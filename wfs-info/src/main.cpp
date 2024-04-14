/*
 * Copyright (C) 2022 koolkdev
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <boost/program_options.hpp>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#include <wfslib/wfslib.h>
#include "../../wfslib/src/area.h"              // TOOD: Public header?
#include "../../wfslib/src/free_blocks_tree.h"  // TOOD: Public header?

std::string inline prettify_path(const std::filesystem::path& path) {
  return "/" + path.generic_string();
}

void dumpArea(int depth, const std::filesystem::path& path, const std::shared_ptr<Area>& area) {
  std::string padding(depth, '\t');
  std::cout << std::format("{}Area {} [0x{:08x}-0x{:08x}]:\n", padding, prettify_path(path), area->BlockNumber(),
                           area->ToAbsoluteBlockNumber(area->BlocksCount()));

  padding += '\t';
  std::shared_ptr<FreeBlocksAllocator> allocator = throw_if_error(area->GetFreeBlocksAllocator());
  FreeBlocksTree tree(allocator.get());
  auto* allocator_header = EPTree(allocator.get()).extra_header();

  std::cout << std::format("{}Free blocks: 0x{:08x}\n", padding, allocator_header->free_blocks_count.value());
  std::cout << std::format("{}Free metadata blocks: 0x{:08x}\n", padding,
                           area->ToAbsoluteBlocksCount(allocator_header->free_blocks_cache_count.value()));
  std::cout << std::format("{}Free metadata block: 0x{:08x}\n", padding,
                           area->ToAbsoluteBlockNumber(allocator_header->free_blocks_cache_count.value()));

  if (depth == 0) {
    auto transactions_area = throw_if_error(area->GetTransactionsArea1());
    std::cout << std::format("{}Transactions area [0x{:08x}-0x{:08x}]\n", padding, transactions_area->BlockNumber(),
                             transactions_area->ToAbsoluteBlockNumber(transactions_area->BlocksCount()));
  }

  std::vector<FreeBlocksRangeInfo> ranges;
  for (const auto& extent : tree) {
    if (!ranges.empty() && ranges.back().block_number + ranges.back().blocks_count == extent.block_number())
      ranges.back().blocks_count += extent.blocks_count();
    else
      ranges.push_back({extent.block_number(), extent.blocks_count()});
  }

  std::cout << std::format("{}Free ranges:\n", padding);
  padding += '\t';
  for (const auto& range : ranges) {
    std::cout << std::format("{}[0x{:08x}-0x{:08x}]\n", padding, area->ToAbsoluteBlockNumber(range.block_number),
                             area->ToAbsoluteBlockNumber(range.block_number + range.blocks_count));
  }
}

void dumpdir(int depth, const std::shared_ptr<Directory>& dir, const std::filesystem::path& path) {
  if (dir->IsQuota()) {
    dumpArea(depth, path, dir->area());
    depth += 1;
  }
  for (auto [name, item_or_error] : *dir) {
    auto const npath = path / name;
    try {
      auto item = throw_if_error(item_or_error);
      if (item->IsDirectory())
        dumpdir(depth, std::dynamic_pointer_cast<Directory>(item), npath);
    } catch (const WfsException& e) {
      std::cout << std::format("Error: Failed to dump {} ({})\n", prettify_path(npath), e.what());
    }
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
    std::string input_path, type;
    std::optional<std::string> seeprom_path, otp_path;

    try {
      boost::program_options::options_description desc("options");
      desc.add_options()("help", "produce help message");

      desc.add_options()("input", boost::program_options::value<std::string>(&input_path)->required(), "input file")(
          "type", boost::program_options::value<std::string>(&type)->default_value("usb")->required(),
          "file type (usb/mlc/plain)")("otp", boost::program_options::value<std::string>(),
                                       "otp file (for usb/mlc types)")(
          "seeprom", boost::program_options::value<std::string>(), "seeprom file (for usb type)");

      boost::program_options::variables_map vm;
      boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);

      if (vm.count("help")) {
        std::cout << "usage: wfs-info --input <input file> [--type <type>]" << std::endl
                  << "                [--otp <path> [--seeprom <path>]]" << std::endl
                  << std::endl
                  << std::endl;
        std::cout << desc << std::endl;
        return 0;
      }

      boost::program_options::notify(vm);

      // Fill arguments
      if (vm.count("otp"))
        otp_path = vm["otp"].as<std::string>();
      if (vm.count("seeprom"))
        seeprom_path = vm["seeprom"].as<std::string>();

      if (type != "usb" && type != "mlc" && type != "plain")
        throw boost::program_options::error("Invalid input type (valid types: usb/mlc/plain)");
      if (type != "plain" && !otp_path)
        throw boost::program_options::error("Missing --otp");
      if (type == "usb" && !seeprom_path)
        throw boost::program_options::error("Missing --seeprom");

    } catch (const boost::program_options::error& e) {
      std::cerr << "Error: " << e.what() << std::endl;
      std::cerr << "Use --help to display program options" << std::endl;
      return 1;
    }

    auto key = get_key(type, otp_path, seeprom_path);
    auto device = std::make_shared<FileDevice>(input_path, 9);
    auto detection_result = Recovery::DetectDeviceParams(device, key);
    if (detection_result.has_value()) {
      if (*detection_result == WfsError::kInvalidWfsVersion)
        std::cerr << "Error: Incorrect WFS version, possible wrong keys";
      else
        throw WfsException(*detection_result);
      return 1;
    }
    std::cout << "Allocator state:" << std::endl;
    dumpdir(0, throw_if_error(Wfs(device, key).GetRootArea()->GetRootDirectory()), {});
    std::cout << "Done!" << std::endl;
  } catch (std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
