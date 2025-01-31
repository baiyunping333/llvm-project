//===-- FileCollectorTest.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "llvm/Support/FileCollector.h"
#include "llvm/Support/FileSystem.h"

using namespace llvm;

namespace llvm {
namespace vfs {
inline bool operator==(const llvm::vfs::YAMLVFSEntry &LHS,
                       const llvm::vfs::YAMLVFSEntry &RHS) {
  return LHS.VPath == RHS.VPath && LHS.RPath == RHS.RPath;
}
} // namespace vfs
} // namespace llvm

namespace {
class TestingFileCollector : public FileCollector {
public:
  using FileCollector::FileCollector;
  using FileCollector::Root;
  using FileCollector::Seen;
  using FileCollector::SymlinkMap;
  using FileCollector::VFSWriter;

  bool hasSeen(StringRef fs) {
    return Seen.find(fs) != Seen.end();
  }
};

struct ScopedDir {
  SmallString<128> Path;
  ScopedDir(const Twine &Name, bool Unique = false) {
    std::error_code EC;
    if (Unique) {
      EC = llvm::sys::fs::createUniqueDirectory(Name, Path);
    } else {
      Path = Name.str();
      EC = llvm::sys::fs::create_directory(Twine(Path));
    }
    if (EC)
      Path = "";
    EXPECT_FALSE(EC);
    // Ensure the path is the real path so tests can use it to compare against
    // realpath output.
    SmallString<128> RealPath;
    if (!llvm::sys::fs::real_path(Path, RealPath))
      Path.swap(RealPath);
  }
  ~ScopedDir() {
    if (Path != "") {
      EXPECT_FALSE(llvm::sys::fs::remove_directories(Path.str()));
    }
  }
  operator StringRef() { return Path.str(); }
};

struct ScopedLink {
  SmallString<128> Path;
  ScopedLink(const Twine &To, const Twine &From) {
    Path = From.str();
    std::error_code EC = sys::fs::create_link(To, From);
    if (EC)
      Path = "";
    EXPECT_FALSE(EC);
  }
  ~ScopedLink() {
    if (Path != "") {
      EXPECT_FALSE(llvm::sys::fs::remove(Path.str()));
    }
  }
  operator StringRef() { return Path.str(); }
};

struct ScopedFile {
  SmallString<128> Path;
  ScopedFile(const Twine &Name) {
    std::error_code EC;
    EC = llvm::sys::fs::createUniqueFile(Name, Path);
    if (EC)
      Path = "";
    EXPECT_FALSE(EC);
  }
  ~ScopedFile() {
    if (Path != "") {
      EXPECT_FALSE(llvm::sys::fs::remove(Path.str()));
    }
  }
  operator StringRef() { return Path.str(); }
};
} // end anonymous namespace

TEST(FileCollectorTest, addFile) {
  ScopedDir root("add_file_root", true);
  std::string root_fs = root.Path.str();
  TestingFileCollector FileCollector(root_fs, root_fs);

  FileCollector.addFile("/path/to/a");
  FileCollector.addFile("/path/to/b");
  FileCollector.addFile("/path/to/c");

  // Make sure the root is correct.
  EXPECT_EQ(FileCollector.Root, root_fs);

  // Make sure we've seen all the added files.
  EXPECT_TRUE(FileCollector.hasSeen("/path/to/a"));
  EXPECT_TRUE(FileCollector.hasSeen("/path/to/b"));
  EXPECT_TRUE(FileCollector.hasSeen("/path/to/c"));

  // Make sure we've only seen the added files.
  EXPECT_FALSE(FileCollector.hasSeen("/path/to/d"));
}

TEST(FileCollectorTest, copyFiles) {
  ScopedDir file_root("file_root", true);
  ScopedFile a(file_root + "/aaa");
  ScopedFile b(file_root + "/bbb");
  ScopedFile c(file_root + "/ccc");

  // Create file collector and add files.
  ScopedDir root("copy_files_root", true);
  std::string root_fs = root.Path.str();
  TestingFileCollector FileCollector(root_fs, root_fs);
  FileCollector.addFile(a.Path);
  FileCollector.addFile(b.Path);
  FileCollector.addFile(c.Path);

  // Make sure we can copy the files.
  std::error_code ec = FileCollector.copyFiles(true);
  EXPECT_FALSE(ec);

  // Now add a bogus file and make sure we error out.
  FileCollector.addFile("/some/bogus/file");
  ec = FileCollector.copyFiles(true);
  EXPECT_TRUE(ec);

  // However, if stop_on_error is true the copy should still succeed.
  ec = FileCollector.copyFiles(false);
  EXPECT_FALSE(ec);
}

#ifndef _WIN32
TEST(FileCollectorTest, Symlinks) {
  // Root where the original files live.
  ScopedDir file_root("file_root", true);

  // Create some files in the file root.
  ScopedFile a(file_root + "/aaa");
  ScopedFile b(file_root + "/bbb");
  ScopedFile c(file_root + "/ccc");

  // Create a directory foo with file ddd.
  ScopedDir foo(file_root + "/foo");
  ScopedFile d(foo + "/ddd");

  // Create a file eee in the foo's parent directory.
  ScopedFile e(foo + "/../eee");

  // Create a symlink bar pointing to foo.
  ScopedLink symlink(file_root + "/foo", file_root + "/bar");

  // Root where files are copied to.
  ScopedDir reproducer_root("reproducer_root", true);
  std::string root_fs = reproducer_root.Path.str();
  TestingFileCollector FileCollector(root_fs, root_fs);

  // Add all the files to the collector.
  FileCollector.addFile(a.Path);
  FileCollector.addFile(b.Path);
  FileCollector.addFile(c.Path);
  FileCollector.addFile(d.Path);
  FileCollector.addFile(e.Path);
  FileCollector.addFile(file_root + "/bar/ddd");

  auto mapping = FileCollector.VFSWriter.getMappings();

  {
    // Make sure the common case works.
    std::string vpath = (file_root + "/aaa").str();
    std::string rpath = (reproducer_root.Path + file_root.Path + "/aaa").str();
    printf("%s -> %s\n", vpath.c_str(), rpath.c_str());
    EXPECT_THAT(mapping, testing::Contains(vfs::YAMLVFSEntry(vpath, rpath)));
  }

  {
    // Make sure the virtual path points to the real source path.
    std::string vpath = (file_root + "/bar/ddd").str();
    std::string rpath =
        (reproducer_root.Path + file_root.Path + "/foo/ddd").str();
    printf("%s -> %s\n", vpath.c_str(), rpath.c_str());
    EXPECT_THAT(mapping, testing::Contains(vfs::YAMLVFSEntry(vpath, rpath)));
  }

  {
    // Make sure that .. is removed from the source path.
    std::string vpath = (file_root + "/eee").str();
    std::string rpath = (reproducer_root.Path + file_root.Path + "/eee").str();
    printf("%s -> %s\n", vpath.c_str(), rpath.c_str());
    EXPECT_THAT(mapping, testing::Contains(vfs::YAMLVFSEntry(vpath, rpath)));
  }
}
#endif
