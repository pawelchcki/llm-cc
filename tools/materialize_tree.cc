#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc < 4 || (argc - 2) % 2 != 0) {
    std::cerr << "usage: materialize_tree OUTPUT SOURCE RELATIVE_DEST [SOURCE "
                 "RELATIVE_DEST ...]\n";
    return 2;
  }

  const fs::path output = argv[1];
  std::error_code error;
  fs::create_directories(output, error);
  if (error) {
    std::cerr << "cannot create " << output << ": " << error.message() << '\n';
    return 1;
  }

  for (int i = 2; i < argc; i += 2) {
    const fs::path source = argv[i];
    const fs::path relative_destination = argv[i + 1];
    if (relative_destination.empty() || relative_destination.is_absolute() ||
        relative_destination.string().find("..") != std::string::npos) {
      std::cerr << "invalid relative destination: " << relative_destination
                << '\n';
      return 2;
    }

    const fs::path destination = output / relative_destination;
    if (fs::is_regular_file(source)) {
      fs::create_directories(destination.parent_path(), error);
      if (!error) {
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing,
                      error);
      }
      if (error) {
        std::cerr << "cannot materialize " << source << " as " << destination
                  << ": " << error.message() << '\n';
        return 1;
      }
      continue;
    }
    fs::create_directories(destination, error);
    if (error) {
      std::cerr << "cannot create " << destination << ": " << error.message()
                << '\n';
      return 1;
    }

    for (const auto& entry : fs::recursive_directory_iterator(source)) {
      const fs::path relative = entry.path().lexically_relative(source);
      if (relative.empty()) {
        std::cerr << "cannot make " << entry.path() << " relative to " << source
                  << '\n';
        return 1;
      }
      const fs::path target = destination / relative;
      if (entry.is_directory()) {
        fs::create_directories(target, error);
      } else if (entry.is_regular_file()) {
        fs::create_directories(target.parent_path(), error);
        if (!error) {
          fs::copy_file(entry.path(), target,
                        fs::copy_options::overwrite_existing, error);
        }
      }
      if (error) {
        std::cerr << "cannot materialize " << entry.path() << " as " << target
                  << ": " << error.message() << '\n';
        return 1;
      }
    }
  }
  return 0;
}
