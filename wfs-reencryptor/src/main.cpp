/*
 * Copyright (C) 2024 koolkdev
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <boost/program_options.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <print>
#include <vector>

#include <wfslib/blocks_device.h>
#include <wfslib/wfslib.h>
// TOOD: Public header?
#include "../../wfslib/src/device_encryption.h"
#include "../../wfslib/src/free_blocks_tree.h"
#include "../../wfslib/src/quota_area.h"

constexpr int kBytesPerGB = 1024 * 1024 * 1024;

class ReencryptorBlocksDevice final : public BlocksDevice {
 public:
  struct BlockInfo {
    uint32_t data_size;
    uint32_t size_in_blocks;
    uint32_t iv;
    bool encrypted;
  };
  ReencryptorBlocksDevice(std::shared_ptr<Device> device, std::optional<std::vector<std::byte>> key = std::nullopt)
      : BlocksDevice(std::move(device), std::move(key)) {}
  ~ReencryptorBlocksDevice() final override = default;

  bool ReadBlock(uint32_t block_number,
                 uint32_t size_in_blocks,
                 const std::span<std::byte>& data,
                 const std::span<const std::byte>& hash,
                 uint32_t iv,
                 bool encrypt,
                 bool check_hash) override {
    assert(check_hash);
    assert(blocks_.find(block_number) == blocks_.end());
    bool res = BlocksDevice::ReadBlock(block_number, size_in_blocks, data, hash, iv, encrypt, check_hash);
    if (!res && block_number == 0)
      return res;
    if (res)
      blocks_[block_number] = BlockInfo{static_cast<uint32_t>(data.size()), size_in_blocks, iv, encrypt};
    else
      bad_blocks_[block_number] = BlockInfo{static_cast<uint32_t>(data.size()), size_in_blocks, iv, encrypt};
    return res;
  }

  size_t blocks_count() const { return blocks_.size(); }

  size_t bytes_count() const {
    return std::accumulate(blocks_.begin(), blocks_.end(), size_t{0},
                           [](size_t acc, const auto& block) { return acc + block.second.data_size; });
  }

  void Reencrypt(std::shared_ptr<BlocksDevice> output_device) {
    auto total_bytes = bytes_count();
    size_t bytes_so_far = 0;
    size_t blocks_so_far = 0;
    for (const auto& [block_number, info] : blocks_) {
      std::vector<std::byte> data(info.data_size, std::byte{0});
      BlocksDevice::ReadBlock(block_number, info.size_in_blocks, data, {}, info.iv, info.encrypted,
                              /*check_hash=*/false);
      output_device->WriteBlock(block_number, info.size_in_blocks, data, {}, info.iv, info.encrypted,
                                /*recalculate_hash=*/false);
      bytes_so_far += info.data_size;
      blocks_so_far++;
      std::print("Reencrypting: {: 8}/{} blocks | {: 6.2f}/{:.2f} GB [{:.2f}%]\r", blocks_so_far, blocks_.size(),
                 static_cast<double>(bytes_so_far) / kBytesPerGB, static_cast<double>(total_bytes) / kBytesPerGB,
                 static_cast<double>(bytes_so_far) / total_bytes * 100);
    }
  }

 private:
  std::map<uint32_t, BlockInfo> blocks_;
  std::map<uint32_t, BlockInfo> bad_blocks_;
};

std::string inline prettify_path(const std::filesystem::path& path) {
  return "/" + path.generic_string();
}

void updateExploreStatus(const ReencryptorBlocksDevice* reencryptor, std::string location) {
  if (location.length() > 64) {
    location = location.substr(0, 64 - 3) + "...";
  }
  std::print("Exploring: {: 6.2f} GB | {: 8} blocks | Exploring: {:64}\r",
             static_cast<double>(reencryptor->bytes_count()) / kBytesPerGB, reencryptor->blocks_count(), location);
}

void exploreDir(const ReencryptorBlocksDevice* reencryptor,
                const std::shared_ptr<Directory>& dir,
                const std::filesystem::path& path,
                bool shadow = false) {
  if (!shadow && dir->is_quota()) {
    // It is an area, iterate its allocator
    auto quota = dir->quota();
    updateExploreStatus(reencryptor, prettify_path(path) + " free blocks allocator");
    try {
      auto allocator = throw_if_error(quota->GetFreeBlocksAllocator());
      FreeBlocksTree tree(allocator.get());
      for ([[maybe_unused]] const auto& extent : tree) {
      }
    } catch (const WfsException& e) {
      std::println(std::cerr, "Error: Failed to explore {} free blocks allocator ({})", prettify_path(path), e.what());
    }
    updateExploreStatus(reencryptor, prettify_path(path) + " shadow dir");
    try {
      exploreDir(reencryptor, throw_if_error(quota->GetShadowDirectory1()), path / ".shadow_dir_1", true);
    } catch (const WfsException& e) {
      std::println(std::cerr, "Error: Failed to explore {} shadow dir 12 ({})", prettify_path(path), e.what());
    }
    try {
      exploreDir(reencryptor, throw_if_error(quota->GetShadowDirectory2()), path / ".shadow_dir_2", true);
    } catch (const WfsException& e) {
      std::println(std::cerr, "Error: Failed to explore {} shadow dir 2 ({})", prettify_path(path), e.what());
    }
  }
  updateExploreStatus(reencryptor, prettify_path(path));
  for (auto [name, entry_or_error] : *dir) {
    auto const npath = path / name;
    try {
      auto entry = throw_if_error(entry_or_error);
      if (entry->is_directory())
        exploreDir(reencryptor, std::dynamic_pointer_cast<Directory>(entry), npath);
      else if (entry->is_file()) {
        updateExploreStatus(reencryptor, prettify_path(npath));
        auto file = std::dynamic_pointer_cast<File>(entry);
        size_t to_read = file->Size();
        File::stream stream(file);
        std::array<char, 0x2000> data;
        while (to_read > 0) {
          stream.read(data.data(), std::min(data.size(), to_read));
          auto read = stream.gcount();
          if (read <= 0) {
            std::println(std::cerr, "Error: Failed to read /{}", npath.generic_string());
            break;
          }
          to_read -= static_cast<size_t>(read);
        }
      }
    } catch (const WfsException& e) {
      std::println(std::cerr, "Error: Failed to explore {} ({})", prettify_path(npath), e.what());
    }
    updateExploreStatus(reencryptor, prettify_path(path));
  }
}

void exploreTransactions(const ReencryptorBlocksDevice* reencryptor, const std::shared_ptr<WfsDevice>& wfs_device) {
  updateExploreStatus(reencryptor, "transactions");
  try {
    throw_if_error(wfs_device->GetTransactionsArea(false));
  } catch (const WfsException& e) {
    std::println(std::cerr, "Error: Failed to explore transactions area 1 ({})", e.what());
  }
  try {
    throw_if_error(wfs_device->GetTransactionsArea(true));
  } catch (const WfsException& e) {
    std::println(std::cerr, "Error: Failed to explore transactions area 2 ({})", e.what());
  }
}

std::optional<std::vector<std::byte>> get_key(std::string type,
                                              std::optional<std::string> otp_path,
                                              std::optional<std::string> seeprom_path) {
  if (type == "mlc") {
    if (!otp_path)
      throw std::runtime_error("missing otp");
    std::unique_ptr<OTP> otp(OTP::LoadFromFile(*otp_path));
    return otp->GetMLCKey();
  } else if (type == "usb") {
    if (!otp_path || !seeprom_path)
      throw std::runtime_error("missing seeprom");
    std::unique_ptr<OTP> otp(OTP::LoadFromFile(*otp_path));
    std::unique_ptr<SEEPROM> seeprom(SEEPROM::LoadFromFile(*seeprom_path));
    return seeprom->GetUSBKey(*otp);
  } else if (type == "plain") {
    return std::nullopt;
  } else {
    throw std::runtime_error("unexpected type");
  }
}

int main(int argc, char* argv[]) {
  try {
    std::string input_path, input_type, output_type;
    std::optional<std::string> output_path, input_seeprom_path, input_otp_path, output_seeprom_path, output_otp_path;

    try {
      boost::program_options::options_description desc("options");
      desc.add_options()("help", "produce help message");

      desc.add_options()("input", boost::program_options::value<std::string>(&input_path)->required(), "input file")(
          "input-type", boost::program_options::value<std::string>(&input_type)->default_value("usb")->required(),
          "input file type (usb/mlc/plain)")("input-otp", boost::program_options::value<std::string>(),
                                             "input otp file (for usb/mlc types)")(
          "input-seeprom", boost::program_options::value<std::string>(), "input seeprom file (for usb type)");

      desc.add_options()("output", boost::program_options::value<std::string>(),
                         "output file (default: reencrypt the input file)")(
          "output-type", boost::program_options::value<std::string>(), "output file type (default: same as input)")(
          "output-otp", boost::program_options::value<std::string>(), "output otp file (for usb/mlc types)")(
          "output-seeprom", boost::program_options::value<std::string>(), "output seeprom file (for usb type)");

      boost::program_options::variables_map vm;
      boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);

      if (vm.count("help")) {
        std::println("usage: wfs-reencryptor --input <input file> [--output <output file>]");
        std::println("                       [--input-type <type>] [--input-otp <path> [--input-seeprom <path>]]");
        std::println("                       [--output-type <type>] [--output-otp <path> [--output-seeprom <path>]]");
        std::println("");
        std::cout << desc << std::endl;
        return 0;
      }

      boost::program_options::notify(vm);

      // Fill arguments

      if (vm.count("output"))
        output_path = vm["output"].as<std::string>();

      output_type = vm.count("output-type") ? vm["output-type"].as<std::string>() : input_type;

      if (vm.count("input-otp"))
        input_otp_path = vm["input-otp"].as<std::string>();
      if (vm.count("input-seeprom"))
        input_seeprom_path = vm["input-seeprom"].as<std::string>();
      if (vm.count("output-otp"))
        output_otp_path = vm["output-otp"].as<std::string>();
      if (vm.count("output-seeprom"))
        output_seeprom_path = vm["output-seeprom"].as<std::string>();

      if (input_type != "usb" && input_type != "mlc" && input_type != "plain")
        throw boost::program_options::error("Invalid input type (valid types: usb/mlc/plain)");
      if (output_type != "usb" && output_type != "mlc" && output_type != "plain")
        throw boost::program_options::error("Invalid output type (valid types: usb/mlc/plain)");
      if (input_type != "plain" && !input_otp_path)
        throw boost::program_options::error("Missing --input-otp");
      if (output_type != "plain" && !output_otp_path)
        throw boost::program_options::error("Missing --output-otp");
      if (input_type == "usb" && !input_seeprom_path)
        throw boost::program_options::error("Missing --input-seeprom");
      if (output_type == "usb" && !output_seeprom_path)
        throw boost::program_options::error("Missing --output-seeprom");

    } catch (const boost::program_options::error& e) {
      std::println(std::cerr, "Error: {}", e.what());
      std::println(std::cerr, "Use --help to display program options");
      return 1;
    }

    auto input_key = get_key(input_type, input_otp_path, input_seeprom_path);
    auto output_key = get_key(output_type, output_otp_path, output_seeprom_path);

    auto input_device = std::make_shared<FileDevice>(input_path, 9, 0, !!output_path);
    auto detection_result = Recovery::DetectDeviceParams(input_device, input_key);
    if (detection_result.has_value()) {
      if (*detection_result == WfsError::kInvalidWfsVersion)
        std::println(std::cerr, "Error: Incorrect WFS version, possible wrong keys");
      else
        throw WfsException(*detection_result);
      return 1;
    }

    auto output_device = output_path ? std::make_shared<FileDevice>(*output_path, input_device->Log2SectorSize(),
                                                                    input_device->SectorsCount(),
                                                                    /*read_only=*/false, /*open_create=*/true)
                                     : input_device;

    auto reencryptor = std::make_shared<ReencryptorBlocksDevice>(input_device, input_key);

    auto wfs_device = throw_if_error(WfsDevice::Open(reencryptor));
    exploreTransactions(reencryptor.get(), wfs_device);
    exploreDir(reencryptor.get(), throw_if_error(wfs_device->GetRootDirectory()), {});
    std::println("");
    reencryptor->Reencrypt(std::make_shared<ReencryptorBlocksDevice>(output_device, output_key));
    std::println("");
    std::println("Done!");
  } catch (std::exception& e) {
    std::println("");
    std::println(std::cerr, "Error: {}", e.what());
    return 1;
  }
  return 0;
}
