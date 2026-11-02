/*****************************************************************/ /**
 * @file   files.hpp
 * @brief  Test helpers for files: a temporary directory, aligned buffers,
 * patterns.
 *********************************************************************/
#ifndef OSPRIM_TESTS_FILES_HPP
#define OSPRIM_TESTS_FILES_HPP

#include <osprim/file.hpp>
#include <osprim/platform.hpp>

#include <doctest/doctest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#if OSPRIM_OS_WINDOWS
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <malloc.h>
  #include <windows.h>
#else
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace osprim_test
{
#if OSPRIM_OS_WINDOWS
  inline std::wstring widen(const std::string& s)
  {
    if (s.empty())
      return {};
    const int n = ::MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
  }

  inline std::string narrow(const wchar_t* w)
  {
    const int n =
        ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1)
      ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
  }

  /// @brief Wine implements some Windows APIs partially.
  inline bool under_wine()
  {
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr
           && ::GetProcAddress(ntdll, "wine_get_version") != nullptr;
  }
#endif

  /// @brief A unique directory under the system temporary directory,
  /// removed with everything created through it.
  class temp_dir
  {
  public:
    temp_dir()
    {
#if OSPRIM_OS_WINDOWS
      wchar_t tmp[MAX_PATH + 1];
      REQUIRE(::GetTempPathW(MAX_PATH + 1, tmp) != 0);
      std::string base = narrow(tmp);
      if (!base.empty() && (base.back() == '\\' || base.back() == '/'))
        base.pop_back();
      static unsigned counter = 0;
      for (;;)
      {
        char name[64];
        ::snprintf(
            name, sizeof(name), "\\osprim_files_%lu_%u", ::GetCurrentProcessId(),
            counter++);
        root_ = base + name;
        if (::CreateDirectoryW(widen(root_).c_str(), nullptr))
          break;
        REQUIRE(::GetLastError() == ERROR_ALREADY_EXISTS);
      }
#else
      // TMPDIR wins when set. Otherwise: Android has no /tmp; what adb
      // shell can write to is /data/local/tmp.
      const char* dir = ::getenv("TMPDIR");
      if (dir == nullptr || *dir == '\0')
      {
  #if defined(__ANDROID__)
        dir = "/data/local/tmp";
  #else
        dir = P_tmpdir;
  #endif
      }
      std::string tmpl = std::string(dir);
      if (!tmpl.empty() && tmpl.back() == '/')
        tmpl.pop_back();
      tmpl += "/osprim_files_XXXXXX";
      REQUIRE(::mkdtemp(tmpl.data()) != nullptr);
      root_ = tmpl;
#endif
    }

    /// @brief Uses `dir` (absolute, existing) instead of a temporary one;
    /// only what is created through it is removed.
    explicit temp_dir(const char* dir, bool)
        : root_(dir)
        , owned_(false)
    {
    }

    ~temp_dir()
    {
      for (size_t i = entries_.size(); i-- > 0;)
        remove_entry(entries_[i]);
      if (owned_)
      {
#if OSPRIM_OS_WINDOWS
        ::RemoveDirectoryW(widen(root_).c_str());
#else
        ::rmdir(root_.c_str());
#endif
      }
    }

    temp_dir(const temp_dir&)            = delete;
    temp_dir& operator=(const temp_dir&) = delete;

    const std::string& root() const noexcept { return root_; }

    /// @brief Absolute path of `name` (removed at the end).
    std::string path(const std::string& name)
    {
      const std::string p = root_ + sep() + name;
      entries_.push_back(entry{p, false});
      return p;
    }

    /// @brief Creates the directory `name` (removed at the end).
    std::string make_dir(const std::string& name)
    {
      const std::string p = root_ + sep() + name;
#if OSPRIM_OS_WINDOWS
      REQUIRE(::CreateDirectoryW((L"\\\\?\\" + widen(p)).c_str(), nullptr));
#else
      REQUIRE(::mkdir(p.c_str(), 0700) == 0);
#endif
      entries_.push_back(entry{p, true});
      return p;
    }

    static const char* sep() noexcept
    {
#if OSPRIM_OS_WINDOWS
      return "\\";
#else
      return "/";
#endif
    }

  private:
    struct entry
    {
      std::string path;
      bool dir;
    };

    static void remove_entry(const entry& e)
    {
#if OSPRIM_OS_WINDOWS
      const std::wstring w = L"\\\\?\\" + widen(e.path);
      if (e.dir)
        ::RemoveDirectoryW(w.c_str());
      else
        ::DeleteFileW(w.c_str());
#else
      if (e.dir)
        ::rmdir(e.path.c_str());
      else
        ::unlink(e.path.c_str());
#endif
    }

    std::string root_;
    std::vector<entry> entries_;
    bool owned_ = true;
  };

  /// @brief Heap memory aligned to `align` bytes.
  class aligned_buffer
  {
  public:
    aligned_buffer(size_t size, size_t align)
        : size_(size)
    {
#if OSPRIM_OS_WINDOWS
      data_ = ::_aligned_malloc(size, align);
#else
      if (::posix_memalign(
              &data_, align < sizeof(void*) ? sizeof(void*) : align, size)
          != 0)
        data_ = nullptr;
#endif
      REQUIRE(data_ != nullptr);
      ::memset(data_, 0, size);
    }
    ~aligned_buffer()
    {
#if OSPRIM_OS_WINDOWS
      ::_aligned_free(data_);
#else
      ::free(data_);
#endif
    }
    aligned_buffer(const aligned_buffer&)            = delete;
    aligned_buffer& operator=(const aligned_buffer&) = delete;

    unsigned char* data() const noexcept
    {
      return static_cast<unsigned char*>(data_);
    }
    size_t size() const noexcept { return size_; }

  private:
    void* data_ = nullptr;
    size_t size_;
  };

  /// @brief The byte at `offset` in the test pattern.
  inline unsigned char pattern_byte(uint64_t offset) noexcept
  {
    return static_cast<unsigned char>(
        (offset * 131u + (offset >> 8) * 7u + 3u) & 0xffu);
  }

  inline void fill_pattern(unsigned char* p, size_t size, uint64_t offset) noexcept
  {
    for (size_t i = 0; i < size; ++i)
      p[i] = pattern_byte(offset + i);
  }

  inline bool has_pattern(
      const unsigned char* p, size_t size, uint64_t offset) noexcept
  {
    for (size_t i = 0; i < size; ++i)
      if (p[i] != pattern_byte(offset + i))
        return false;
    return true;
  }

  inline bool all_zero(const unsigned char* p, size_t size) noexcept
  {
    for (size_t i = 0; i < size; ++i)
      if (p[i] != 0)
        return false;
    return true;
  }

  /// @brief Opens `path`, requiring success.
  inline osprim::file_handle open_ok(
      const std::string& path, osprim::file_access access,
      osprim::file_disposition disposition =
          osprim::file_disposition::FILE_DISPOSITION_OPEN_EXISTING)
  {
    osprim::file_open_desc desc;
    desc.access      = access;
    desc.disposition = disposition;
    osprim::file_handle h{};
    const auto s = osprim::file_open(path.data(), path.size(), desc, h);
    INFO("path: " << path << " native_code: " << s.native_code);
    REQUIRE(s.ok());
    return h;
  }

  /// @brief Creates `path` holding `size` pattern bytes, returns it open for reading and writing.
  inline osprim::file_handle make_pattern_file(const std::string& path, size_t size)
  {
    const osprim::file_handle h = open_ok(
        path, osprim::file_access::FILE_ACCESS_READ_WRITE,
        osprim::file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE);
    std::vector<unsigned char> data(size);
    fill_pattern(data.data(), size, 0);
    size_t done = 0;
    if (size != 0)
    {
      const auto s = osprim::file_write_at(h, 0, data.data(), size, done);
      REQUIRE(s.ok());
      REQUIRE(done == size);
    }
    return h;
  }

  /// @brief Reads `size` bytes at `offset`, requiring success.
  inline std::vector<unsigned char> read_all(
      osprim::file_handle h, uint64_t offset, size_t size)
  {
    std::vector<unsigned char> data(size);
    size_t done  = 0;
    const auto s = osprim::file_read_at(h, offset, data.data(), size, done);
    REQUIRE(s.ok());
    data.resize(done);
    return data;
  }
} // namespace osprim_test

#endif // !OSPRIM_TESTS_FILES_HPP
