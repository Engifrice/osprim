/*****************************************************************/ /**
 * @file   nt.hpp
 * @brief  Internal (Windows): the few NT native API types, constants and
 * entry points the library uses, declared here so that neither winternl.h
 * nor the WDK is needed. The entry points are resolved from ntdll.dll by
 * platform_file_initialize().
 *********************************************************************/
#ifndef OSPRIM_SRC_NT_HPP
#define OSPRIM_SRC_NT_HPP

#include "platform.hpp"

#if !OSPRIM_OS_WINDOWS
  #error "nt.hpp is for Windows only"
#endif

#include "win32.hpp"

namespace osprim
{
  inline namespace v0_1
  {
    namespace detail
    {
      struct nt_unicode_string
      {
        USHORT Length; ///< Bytes, without terminator
        USHORT MaximumLength;
        PWSTR Buffer;
      };

      struct nt_object_attributes
      {
        ULONG Length;
        HANDLE RootDirectory;
        nt_unicode_string* ObjectName;
        ULONG Attributes;
        PVOID SecurityDescriptor;
        PVOID SecurityQualityOfService;
      };

      struct nt_io_status_block
      {
        union
        {
          LONG Status;
          PVOID Pointer;
        };
        ULONG_PTR Information;
      };

      using nt_create_file_fn = LONG(NTAPI*)(
          HANDLE*, ACCESS_MASK, nt_object_attributes*, nt_io_status_block*,
          LARGE_INTEGER*, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
      using nt_query_information_file_fn =
          LONG(NTAPI*)(HANDLE, nt_io_status_block*, PVOID, ULONG, ULONG);

      /// @brief ntdll entry points (null if missing).
      struct nt_api
      {
        nt_create_file_fn create_file;
        nt_query_information_file_fn query_information_file;
      };

      extern nt_api g_nt;

      // OBJECT_ATTRIBUTES::Attributes
      constexpr ULONG NT_OBJ_CASE_INSENSITIVE = 0x00000040;

      // CreateDisposition
      constexpr ULONG NT_FILE_OPEN         = 1;
      constexpr ULONG NT_FILE_CREATE       = 2;
      constexpr ULONG NT_FILE_OPEN_IF      = 3;
      constexpr ULONG NT_FILE_OVERWRITE    = 4;
      constexpr ULONG NT_FILE_OVERWRITE_IF = 5;

      // IO_STATUS_BLOCK::Information after a create
      constexpr ULONG_PTR NT_FILE_CREATED = 2;

      // CreateOptions
      constexpr ULONG NT_FILE_DIRECTORY_FILE            = 0x00000001;
      constexpr ULONG NT_FILE_SEQUENTIAL_ONLY           = 0x00000004;
      constexpr ULONG NT_FILE_NO_INTERMEDIATE_BUFFERING = 0x00000008;
      constexpr ULONG NT_FILE_SYNCHRONOUS_IO_NONALERT   = 0x00000020;
      constexpr ULONG NT_FILE_NON_DIRECTORY_FILE        = 0x00000040;
      constexpr ULONG NT_FILE_RANDOM_ACCESS             = 0x00000800;
      constexpr ULONG NT_FILE_OPEN_REPARSE_POINT        = 0x00200000;

      // FILE_INFORMATION_CLASS (NtQueryInformationFile)
      constexpr ULONG NT_FILE_MODE_INFORMATION = 16;

      // NTSTATUS values
      constexpr LONG NT_STATUS_SUCCESS             = 0;
      constexpr LONG NT_STATUS_STOPPED_ON_SYMLINK  = static_cast<LONG>(0x8000002DL);
      constexpr LONG NT_STATUS_NO_SUCH_FILE        = static_cast<LONG>(0xC000000FL);
      constexpr LONG NT_STATUS_INVALID_PARAMETER   = static_cast<LONG>(0xC000000DL);
      constexpr LONG NT_STATUS_ACCESS_DENIED       = static_cast<LONG>(0xC0000022L);
      constexpr LONG NT_STATUS_OBJECT_NAME_INVALID = static_cast<LONG>(0xC0000033L);
      constexpr LONG NT_STATUS_OBJECT_NAME_NOT_FOUND =
          static_cast<LONG>(0xC0000034L);
      constexpr LONG NT_STATUS_OBJECT_NAME_COLLISION =
          static_cast<LONG>(0xC0000035L);
      constexpr LONG NT_STATUS_OBJECT_PATH_INVALID = static_cast<LONG>(0xC0000039L);
      constexpr LONG NT_STATUS_OBJECT_PATH_NOT_FOUND =
          static_cast<LONG>(0xC000003AL);
      constexpr LONG NT_STATUS_OBJECT_PATH_SYNTAX_BAD =
          static_cast<LONG>(0xC000003BL);
      constexpr LONG NT_STATUS_SHARING_VIOLATION = static_cast<LONG>(0xC0000043L);
      constexpr LONG NT_STATUS_DELETE_PENDING    = static_cast<LONG>(0xC0000056L);
      constexpr LONG NT_STATUS_DISK_FULL         = static_cast<LONG>(0xC000007FL);
      constexpr LONG NT_STATUS_MEDIA_WRITE_PROTECTED =
          static_cast<LONG>(0xC00000A2L);
      constexpr LONG NT_STATUS_FILE_IS_A_DIRECTORY = static_cast<LONG>(0xC00000BAL);
      constexpr LONG NT_STATUS_NOT_SUPPORTED       = static_cast<LONG>(0xC00000BBL);
      constexpr LONG NT_STATUS_NOT_SAME_DEVICE     = static_cast<LONG>(0xC00000D4L);
      constexpr LONG NT_STATUS_DIRECTORY_NOT_EMPTY = static_cast<LONG>(0xC0000101L);
      constexpr LONG NT_STATUS_NOT_A_DIRECTORY     = static_cast<LONG>(0xC0000103L);
      constexpr LONG NT_STATUS_NAME_TOO_LONG       = static_cast<LONG>(0xC0000106L);
      constexpr LONG NT_STATUS_TOO_MANY_OPENED_FILES =
          static_cast<LONG>(0xC000011FL);
      constexpr LONG NT_STATUS_CANNOT_DELETE = static_cast<LONG>(0xC0000121L);
      constexpr LONG NT_STATUS_REPARSE_POINT_NOT_RESOLVED =
          static_cast<LONG>(0xC0000280L);
      constexpr LONG NT_STATUS_REPARSE_POINT_ENCOUNTERED =
          static_cast<LONG>(0xC000050BL);
    } // namespace detail
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_SRC_NT_HPP
