/*
 * This file is part of Foreign Linux.
 *
 * Copyright (C) 2014, 2015 Xiangyan Sun <wishstudio@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <common/errno.h>
#include <common/fcntl.h>
#include <common/fs.h>
#include <fs/winfs.h>
#include <syscall/mm.h>
#include <syscall/vfs.h>
#include <datetime.h>
#include <heap.h>
#include <log.h>
#include <str.h>

#include <ntdll.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <limits.h>

#define WINFS_SYMLINK_HEADER		"!<SYMLINK>\379\378"
#define WINFS_SYMLINK_HEADER_LEN	(sizeof(WINFS_SYMLINK_HEADER) - 1)

struct winfs_file
{
	struct file base_file;
	HANDLE handle;
	int restart_scan; /* for getdents() */
	int pathlen;
	char pathname[]; /* Not necessary null-terminated */
};

/* Convert an utf-8 file name to NT file name, return converted name length in characters, no NULL terminator is appended */
static int filename_to_nt_pathname(const char *filename, WCHAR *buf, int buf_size)
{
	if (buf_size < 4)
		return 0;
	buf[0] = L'\\';
	buf[1] = L'?';
	buf[2] = L'?';
	buf[3] = L'\\';
	buf += 4;
	buf_size -= 4;
	int out_size = 4;
	int len = (DWORD)GetCurrentDirectoryW(buf_size, buf);
	buf += len;
	out_size += len;
	buf_size -= len;
	if (filename[0] == 0)
		return out_size;
	*buf++ = L'\\';
	out_size++;
	buf_size--;
	int fl = utf8_to_utf16_filename(filename, strlen(filename), buf, buf_size);
	if (fl == 0)
		return 0;
	return out_size + fl;
}

static int cached_sid_initialized;
static char cached_sid_buffer[256];
static PSID cached_sid;

/* TODO: This function should be placed in a better place */
static PSID get_user_sid()
{
	if (cached_sid_initialized)
		return cached_sid;
	else
	{
		HANDLE token;
		OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
		DWORD len;
		GetTokenInformation(token, TokenUser, cached_sid_buffer, sizeof(cached_sid_buffer), &len);
		TOKEN_USER *user = (TOKEN_USER *)cached_sid_buffer;
		cached_sid = user->User.Sid;
		cached_sid_initialized = 1;
		CloseHandle(token);
		return cached_sid;
	}
}

/* Move a file handle to recycle bin
 * The pathname must be a valid NT file name generated using filename_to_nt_pathname()
 */
static NTSTATUS move_to_recycle_bin(HANDLE handle, WCHAR *pathname)
{
	IO_STATUS_BLOCK status_block;
	NTSTATUS status;

	/* TODO: Handle the case when recycle bin does not exist (according to cygwin) */
	/* TODO: Handle when the file is inside recycle bin */
	WCHAR recyclepath[512];
	UNICODE_STRING recycle;
	RtlInitEmptyUnicodeString(&recycle, recyclepath, sizeof(recyclepath));
	/* Root directory, should look like "\??\C:\", 7 characters */
	UNICODE_STRING root;
	RtlInitCountedUnicodeString(&root, pathname, sizeof(WCHAR) * 7);
	RtlAppendUnicodeStringToString(&recycle, &root);
	RtlAppendUnicodeToString(&recycle, L"$Recycle.Bin\\");

	WCHAR renamepath[512];
	UNICODE_STRING rename;
	RtlInitEmptyUnicodeString(&rename, renamepath, sizeof(renamepath));
	RtlAppendUnicodeStringToString(&rename, &recycle);
	/* Append user sid */
	{
		WCHAR buf[256];
		UNICODE_STRING sid;
		RtlInitEmptyUnicodeString(&sid, buf, sizeof(buf));
		RtlConvertSidToUnicodeString(&sid, get_user_sid(), FALSE);
		RtlAppendUnicodeStringToString(&rename, &sid);
		RtlAppendUnicodeToString(&rename, L"\\");
	}
	/* Generate an unique file name by append file id and a hash of the pathname,
	 * To allow unlinking multiple hard links of the same file
	 */
	RtlAppendUnicodeToString(&rename, L".flinux");
	/* Append file id */
	{
		FILE_INTERNAL_INFORMATION info;
		status = NtQueryInformationFile(handle, &status_block, &info, sizeof(info), FileInternalInformation);
		if (!NT_SUCCESS(status))
		{
			log_error("NtQueryInformationFile(FileInternalInformation) failed, status: %x\n", status);
			return status;
		}
		RtlAppendInt64ToString(info.IndexNumber.QuadPart, 16, &rename);
		RtlAppendUnicodeToString(&rename, L"_");
	}
	/* Append file path hash */
	{
		UNICODE_STRING path;
		RtlInitUnicodeString(&path, pathname);
		ULONG hash;
		RtlHashUnicodeString(&path, FALSE, HASH_STRING_ALGORITHM_DEFAULT, &hash);
		RtlAppendIntegerToString(hash, 16, &rename);
	}
	/* Rename file */
	char buf[512];
	FILE_RENAME_INFORMATION *info = (FILE_RENAME_INFORMATION *)buf;
	info->ReplaceIfExists = FALSE;
	info->RootDirectory = NULL;
	info->FileNameLength = rename.Length;
	memcpy(info->FileName, rename.Buffer, rename.Length);
	status = NtSetInformationFile(handle, &status_block, info, sizeof(*info) + info->FileNameLength, FileRenameInformation);
	if (!NT_SUCCESS(status))
	{
		log_error("NtSetInformationFile(FileRenameInformation) failed, status: %x\n", status);
		return status;
	}
	return STATUS_SUCCESS;
}

/* Test if a handle is a symlink, does not read the target
 * The current file pointer will be changed
 */
static int winfs_is_symlink(HANDLE hFile)
{
	char header[WINFS_SYMLINK_HEADER_LEN];
	DWORD num_read;
	OVERLAPPED overlapped;
	overlapped.Internal = 0;
	overlapped.InternalHigh = 0;
	overlapped.Offset = 0;
	overlapped.OffsetHigh = 0;
	overlapped.hEvent = 0;
	if (!ReadFile(hFile, header, WINFS_SYMLINK_HEADER_LEN, &num_read, &overlapped) || num_read < WINFS_SYMLINK_HEADER_LEN)
	{
		log_error("ReadFile(): %d\n", GetLastError());
		return 0;
	}
	if (memcmp(header, WINFS_SYMLINK_HEADER, WINFS_SYMLINK_HEADER_LEN))
		return 0;
	return 1;
}

/*
Test if a handle is a symlink, also return its target if requested.
For optimal performance, caller should ensure the handle is a regular file with system attribute.
*/
static int winfs_read_symlink(HANDLE hFile, char *target, int buflen)
{
	char header[WINFS_SYMLINK_HEADER_LEN];
	DWORD num_read;
	/* Use overlapped structure to avoid changing file pointer */
	OVERLAPPED overlapped;
	overlapped.Internal = 0;
	overlapped.InternalHigh = 0;
	overlapped.Offset = 0;
	overlapped.OffsetHigh = 0;
	overlapped.hEvent = 0;
	if (!ReadFile(hFile, header, WINFS_SYMLINK_HEADER_LEN, &num_read, &overlapped) || num_read < WINFS_SYMLINK_HEADER_LEN)
		return 0;
	if (memcmp(header, WINFS_SYMLINK_HEADER, WINFS_SYMLINK_HEADER_LEN))
		return 0;
	if (target == NULL || buflen == 0)
	{
		LARGE_INTEGER size;
		if (!GetFileSizeEx(hFile, &size) || size.QuadPart - WINFS_SYMLINK_HEADER_LEN >= PATH_MAX)
			return 0;
		return (int)size.QuadPart - WINFS_SYMLINK_HEADER_LEN;
	}
	else
	{
		overlapped.Offset = WINFS_SYMLINK_HEADER_LEN;
		if (!ReadFile(hFile, target, buflen, &num_read, &overlapped))
			return 0;
		target[num_read] = 0;
		return num_read;
	}
}

static int winfs_close(struct file *f)
{
	struct winfs_file *winfile = (struct winfs_file *)f;
	if (CloseHandle(winfile->handle))
	{
		kfree(winfile, sizeof(struct winfs_file) + winfile->pathlen);
		return 0;
	}
	else
		return -1;
}

static int winfs_getpath(struct file *f, char *buf)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *)f;
	buf[0] = '/'; /* the mountpoint */
	memcpy(buf + 1, winfile->pathname, winfile->pathlen);
	buf[1 + winfile->pathlen] = 0;
	int r = winfile->pathlen + 1;
	ReleaseSRWLockShared(&f->rw_lock);
	return r;
}

static size_t winfs_read(struct file *f, void *buf, size_t count)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *) f;
	size_t num_read = 0;
	while (count > 0)
	{
		DWORD count_dword = (DWORD)min(count, (size_t)UINT_MAX);
		DWORD num_read_dword;
		if (!ReadFile(winfile->handle, buf, count_dword, &num_read_dword, NULL))
		{
			if (GetLastError() == ERROR_HANDLE_EOF)
				break;
			log_warning("ReadFile() failed, error code: %d\n", GetLastError());
			num_read = -EIO;
			break;
		}
		if (num_read_dword == 0)
			break;
		num_read += num_read_dword;
		count -= num_read_dword;
	}
	ReleaseSRWLockShared(&f->rw_lock);
	return num_read;
}

static size_t winfs_write(struct file *f, const void *buf, size_t count)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *) f;
	size_t num_written = 0;
	OVERLAPPED overlapped;
	overlapped.Internal = 0;
	overlapped.InternalHigh = 0;
	overlapped.Offset = 0xFFFFFFFF;
	overlapped.OffsetHigh = 0xFFFFFFFF;
	overlapped.hEvent = NULL;
	OVERLAPPED *overlapped_pointer = (f->flags & O_APPEND)? &overlapped: NULL;
	while (count > 0)
	{
		DWORD count_dword = (DWORD)min(count, (size_t)UINT_MAX);
		DWORD num_written_dword;
		if (!WriteFile(winfile->handle, buf, count_dword, &num_written_dword, overlapped_pointer))
		{
			log_warning("WriteFile() failed, error code: %d\n", GetLastError());
			num_written = -EIO;
			break;
		}
		num_written += num_written_dword;
		count -= num_written_dword;
	}
	ReleaseSRWLockShared(&f->rw_lock);
	return num_written;
}

static size_t winfs_pread(struct file *f, void *buf, size_t count, loff_t offset)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *) f;
	size_t num_read = 0;
	while (count > 0)
	{
		OVERLAPPED overlapped;
		overlapped.Internal = 0;
		overlapped.InternalHigh = 0;
		overlapped.Offset = offset & 0xFFFFFFFF;
		overlapped.OffsetHigh = offset >> 32ULL;
		overlapped.hEvent = 0;
		DWORD count_dword = (DWORD)min(count, (size_t)UINT_MAX);
		DWORD num_read_dword;
		if (!ReadFile(winfile->handle, buf, count_dword, &num_read_dword, &overlapped))
		{
			if (GetLastError() == ERROR_HANDLE_EOF)
				break;
			log_warning("ReadFile() failed, error code: %d\n", GetLastError());
			num_read = -EIO;
			break;
		}
		if (num_read_dword == 0)
			break;
		num_read += num_read_dword;
		offset += num_read_dword;
		count -= num_read_dword;
	}
	ReleaseSRWLockShared(&f->rw_lock);
	return num_read;
}

static size_t winfs_pwrite(struct file *f, const void *buf, size_t count, loff_t offset)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *) f;
	size_t num_written = 0;
	while (count > 0)
	{
		OVERLAPPED overlapped;
		overlapped.Internal = 0;
		overlapped.InternalHigh = 0;
		overlapped.Offset = offset & 0xFFFFFFFF;
		overlapped.OffsetHigh = offset >> 32ULL;
		overlapped.hEvent = 0;
		DWORD count_dword = (DWORD)min(count, (size_t)UINT_MAX);
		DWORD num_written_dword;
		if (!WriteFile(winfile->handle, buf, count_dword, &num_written_dword, &overlapped))
		{
			log_warning("WriteFile() failed, error code: %d\n", GetLastError());
			num_written = -EIO;
			break;
		}
		num_written += num_written_dword;
		offset += num_written_dword;
		count -= num_written_dword;
	}
	ReleaseSRWLockShared(&f->rw_lock);
	return num_written;
}

static size_t winfs_readlink(struct file *f, char *target, size_t buflen)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *) f;
	int r = winfs_read_symlink(winfile->handle, target, (int)buflen);
	ReleaseSRWLockShared(&f->rw_lock);
	if (r == 0)
		return -EINVAL;
	return r;
}

static int winfs_truncate(struct file *f, loff_t length)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *) f;
	/* TODO: Correct errno */
	FILE_END_OF_FILE_INFORMATION info;
	info.EndOfFile.QuadPart = length;
	IO_STATUS_BLOCK status_block;
	NTSTATUS status;
	status = NtSetInformationFile(winfile->handle, &status_block, &info, sizeof(info), FileEndOfFileInformation);
	ReleaseSRWLockShared(&f->rw_lock);
	if (!NT_SUCCESS(status))
	{
		log_warning("NtSetInformationFile(FileEndOfFileInformation) failed, status: %x\n", status);
		return -EIO;
	}
	return 0;
}

static int winfs_fsync(struct file *f)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *) f;
	BOOL ok = FlushFileBuffers(winfile->handle);
	ReleaseSRWLockShared(&f->rw_lock);
	if (!ok)
	{
		log_warning("FlushFileBuffers() failed, error code: %d\n", GetLastError());
		return -EIO;
	}
	return 0;
}

static int winfs_llseek(struct file *f, loff_t offset, loff_t *newoffset, int whence)
{
	struct winfs_file *winfile = (struct winfs_file *) f;
	DWORD dwMoveMethod;
	if (whence == SEEK_SET)
		dwMoveMethod = FILE_BEGIN;
	else if (whence == SEEK_CUR)
		dwMoveMethod = FILE_CURRENT;
	else if (whence == SEEK_END)
		dwMoveMethod = FILE_END;
	else
		return -EINVAL;
	AcquireSRWLockExclusive(&f->rw_lock);
	LARGE_INTEGER liDistanceToMove, liNewFilePointer;
	liDistanceToMove.QuadPart = offset;
	SetFilePointerEx(winfile->handle, liDistanceToMove, &liNewFilePointer, dwMoveMethod);
	*newoffset = liNewFilePointer.QuadPart;
	if (whence == SEEK_SET && offset == 0)
	{
		/* TODO: Currently we don't know if it is a directory, pretend it is */
		winfile->restart_scan = 1;
	}
	ReleaseSRWLockExclusive(&f->rw_lock);
	return 0;
}

static int winfs_stat(struct file *f, struct newstat *buf)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *) f;
	BY_HANDLE_FILE_INFORMATION info;
	GetFileInformationByHandle(winfile->handle, &info);

	/* Programs (ld.so) may use st_dev and st_ino to identity files so these must be unique for each file. */
	INIT_STRUCT_NEWSTAT_PADDING(buf);
	buf->st_dev = mkdev(8, 0); // (8, 0): /dev/sda
	//buf->st_ino = ((uint64_t)info.nFileIndexHigh << 32ULL) + info.nFileIndexLow;
	/* Hash 64 bit inode to 32 bit to fix legacy applications
	 * We may later add an option for changing this behaviour
	 */
	buf->st_ino = info.nFileIndexHigh ^ info.nFileIndexLow;
	if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
		buf->st_mode = 0555;
	else
		buf->st_mode = 0755;
	if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
	{
		buf->st_mode |= S_IFDIR;
		buf->st_size = 0;
	}
	else
	{
		int r;
		if ((info.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)
			&& (r = winfs_read_symlink(winfile->handle, NULL, 0)) > 0)
		{
			buf->st_mode |= S_IFLNK;
			buf->st_size = r;
		}
		else
		{
			buf->st_mode |= S_IFREG;
			buf->st_size = ((uint64_t)info.nFileSizeHigh << 32ULL) + info.nFileSizeLow;
		}
	}
	buf->st_nlink = info.nNumberOfLinks;
	buf->st_uid = 0;
	buf->st_gid = 0;
	buf->st_rdev = 0;
	buf->st_blksize = PAGE_SIZE;
	buf->st_blocks = (buf->st_size + buf->st_blksize - 1) / buf->st_blksize;
	buf->st_atime = filetime_to_unix_sec(&info.ftLastAccessTime);
	buf->st_atime_nsec = filetime_to_unix_nsec(&info.ftLastAccessTime);
	buf->st_mtime = filetime_to_unix_sec(&info.ftLastWriteTime);
	buf->st_mtime_nsec = filetime_to_unix_nsec(&info.ftLastWriteTime);
	buf->st_ctime = filetime_to_unix_sec(&info.ftCreationTime);
	buf->st_ctime_nsec = filetime_to_unix_nsec(&info.ftCreationTime);
	ReleaseSRWLockShared(&f->rw_lock);
	return 0;
}

static int winfs_utimens(struct file *f, const struct timespec *times)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfs = (struct winfs_file *)f;
	if (!times)
	{
		SYSTEMTIME time;
		GetSystemTime(&time);
		FILETIME filetime;
		SystemTimeToFileTime(&time, &filetime);
		SetFileTime(winfs->handle, NULL, &filetime, &filetime);
	}
	else
	{
		FILETIME actime, modtime;
		unix_timespec_to_filetime(&times[0], &actime);
		unix_timespec_to_filetime(&times[1], &modtime);
		SetFileTime(winfs->handle, NULL, &actime, &modtime);
	}
	ReleaseSRWLockShared(&f->rw_lock);
	return 0;
}

static int winfs_getdents(struct file *f, void *dirent, size_t count, getdents_callback *fill_callback)
{
	AcquireSRWLockShared(&f->rw_lock);
	NTSTATUS status;
	struct winfs_file *winfile = (struct winfs_file *) f;
	IO_STATUS_BLOCK status_block;
	#define BUFFER_SIZE	32768
	char buffer[BUFFER_SIZE];
	int size = 0;

	for (;;)
	{
		/* sizeof(FILE_ID_FULL_DIR_INFORMATION) is larger than both sizeof(struct dirent) and sizeof(struct dirent64)
		 * So we don't need to worry about header size.
		 * For the file name, in worst case, a UTF-16 character (2 bytes) requires 4 bytes to store */
		int buffer_size = (count - size) / 2;
		if (buffer_size >= BUFFER_SIZE)
			buffer_size = BUFFER_SIZE;
		status = NtQueryDirectoryFile(winfile->handle, NULL, NULL, NULL, &status_block, buffer, buffer_size, FileIdFullDirectoryInformation, FALSE, NULL, winfile->restart_scan);
		winfile->restart_scan = 0;
		if (!NT_SUCCESS(status))
		{
			if (status != STATUS_NO_MORE_FILES)
				log_error("NtQueryDirectoryFile() failed, status: %x\n", status);
			break;
		}
		if (status_block.Information == 0)
			break;
		int offset = 0;
		FILE_ID_FULL_DIR_INFORMATION *info;
		do
		{
			info = (FILE_ID_FULL_DIR_INFORMATION *) &buffer[offset];
			offset += info->NextEntryOffset;
			void *p = (char *)dirent + size;
			//uint64_t inode = info->FileId.QuadPart;
			/* Hash 64 bit inode to 32 bit to fix legacy applications
			 * We may later add an option for changing this behaviour
			 */
			uint64_t inode = info->FileId.HighPart ^ info->FileId.LowPart;
			char type = DT_REG;
			if (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				type = DT_DIR;
			else if (info->FileAttributes & FILE_ATTRIBUTE_SYSTEM)
			{
				/* Test if it is a symlink */
				UNICODE_STRING pathname;
				pathname.Length = info->FileNameLength;
				pathname.MaximumLength = info->FileNameLength;
				pathname.Buffer = info->FileName;

				NTSTATUS status;
				IO_STATUS_BLOCK status_block;
				OBJECT_ATTRIBUTES attr;
				attr.Length = sizeof(OBJECT_ATTRIBUTES);
				attr.RootDirectory = winfile->handle;
				attr.ObjectName = &pathname;
				attr.Attributes = 0;
				attr.SecurityDescriptor = NULL;
				attr.SecurityQualityOfService = NULL;
				HANDLE handle;
				status = NtCreateFile(&handle, SYNCHRONIZE | FILE_READ_DATA, &attr, &status_block, NULL,
					FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
					FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
				if (NT_SUCCESS(status))
				{
					if (winfs_is_symlink(handle))
						type = DT_LNK;
					NtClose(handle);
				}
				else
					log_warning("NtCreateFile() failed, status: %x\n", status);
			}
			intptr_t reclen = fill_callback(p, inode, info->FileName, info->FileNameLength / 2, type, count - size, GETDENTS_UTF16);
			if (reclen < 0)
			{
				size = reclen;
				goto out;
			}
			size += reclen;
		} while (info->NextEntryOffset);
	}
out:
	ReleaseSRWLockShared(&f->rw_lock);
	return size;
	#undef BUFFER_SIZE
}

static int winfs_statfs(struct file *f, struct statfs64 *buf)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *) f;
	FILE_FS_FULL_SIZE_INFORMATION info;
	IO_STATUS_BLOCK status_block;
	NTSTATUS status = NtQueryVolumeInformationFile(winfile->handle, &status_block, &info, sizeof(info), FileFsFullSizeInformation);
	int r = 0;
	if (!NT_SUCCESS(status))
	{
		log_warning("NtQueryVolumeInformationFile() failed, status: %x\n", status);
		r = -EIO;
		goto out;
	}
	buf->f_type = 0x5346544e; /* NTFS_SB_MAGIC */
	buf->f_bsize = info.SectorsPerAllocationUnit * info.BytesPerSector;
	buf->f_blocks = info.TotalAllocationUnits.QuadPart;
	buf->f_bfree = info.ActualAvailableAllocationUnits.QuadPart;
	buf->f_bavail = info.CallerAvailableAllocationUnits.QuadPart;
	buf->f_files = 0;
	buf->f_ffree = 0;
	buf->f_fsid.val[0] = 0;
	buf->f_fsid.val[1] = 0;
	buf->f_namelen = PATH_MAX;
	buf->f_frsize = 0;
	buf->f_flags = 0;
	buf->f_spare[0] = 0;
	buf->f_spare[1] = 0;
	buf->f_spare[2] = 0;
	buf->f_spare[3] = 0;
out:
	ReleaseSRWLockShared(&f->rw_lock);
	return r;
}

static struct file_ops winfs_ops = 
{
	.close = winfs_close,
	.getpath = winfs_getpath,
	.read = winfs_read,
	.write = winfs_write,
	.pread = winfs_pread,
	.pwrite = winfs_pwrite,
	.readlink = winfs_readlink,
	.truncate = winfs_truncate,
	.fsync = winfs_fsync,
	.llseek = winfs_llseek,
	.stat = winfs_stat,
	.utimens = winfs_utimens,
	.getdents = winfs_getdents,
	.statfs = winfs_statfs,
};

static int winfs_symlink(struct file_system *fs, const char *target, const char *linkpath)
{
	HANDLE handle;
	WCHAR wlinkpath[PATH_MAX];

	if (utf8_to_utf16_filename(linkpath, strlen(linkpath) + 1, wlinkpath, PATH_MAX) <= 0)
		return -ENOENT;

	log_info("CreateFileW(): %s\n", linkpath);
	handle = CreateFileW(wlinkpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_NEW, FILE_ATTRIBUTE_SYSTEM, NULL);
	if (handle == INVALID_HANDLE_VALUE)
	{
		DWORD err = GetLastError();
		if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS)
		{
			log_warning("File already exists.\n");
			return -EEXIST;
		}
		log_warning("CreateFileW() failed, error code: %d.\n", GetLastError());
		return -ENOENT;
	}
	DWORD num_written;
	if (!WriteFile(handle, WINFS_SYMLINK_HEADER, WINFS_SYMLINK_HEADER_LEN, &num_written, NULL) || num_written < WINFS_SYMLINK_HEADER_LEN)
	{
		log_warning("WriteFile() failed, error code: %d.\n", GetLastError());
		CloseHandle(handle);
		return -EIO;
	}
	DWORD targetlen = strlen(target);
	if (!WriteFile(handle, target, targetlen, &num_written, NULL) || num_written < targetlen)
	{
		log_warning("WriteFile() failed, error code: %d.\n", GetLastError());
		CloseHandle(handle);
		return -EIO;
	}
	CloseHandle(handle);
	return 0;
}

static int winfs_link(struct file_system *fs, struct file *f, const char *newpath)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *) f;
	NTSTATUS status;
	int r = 0;
	char buf[sizeof(FILE_LINK_INFORMATION) + PATH_MAX * 2];
	FILE_LINK_INFORMATION *info = (FILE_LINK_INFORMATION *)buf;
	info->ReplaceIfExists = FALSE;
	info->RootDirectory = NULL;
	info->FileNameLength = 2 * filename_to_nt_pathname(newpath, info->FileName, PATH_MAX);
	if (info->FileNameLength == 0)
	{
		r = -ENOENT;
		goto out;
	}
	IO_STATUS_BLOCK status_block;
	status = NtSetInformationFile(winfile->handle, &status_block, info, info->FileNameLength + sizeof(FILE_LINK_INFORMATION), FileLinkInformation);
	if (!NT_SUCCESS(status))
	{
		log_warning("NtSetInformationFile() failed, status: %x.\n", status);
		r = -ENOENT;
		goto out;
	}
out:
	ReleaseSRWLockShared(&f->rw_lock);
	return r;
}

static int winfs_unlink(struct file_system *fs, const char *pathname)
{
	WCHAR wpathname[PATH_MAX];
	int len = filename_to_nt_pathname(pathname, wpathname, PATH_MAX);
	if (len <= 0)
		return -ENOENT;

	UNICODE_STRING object_name;
	RtlInitCountedUnicodeString(&object_name, wpathname, len * sizeof(WCHAR));

	OBJECT_ATTRIBUTES attr;
	attr.Length = sizeof(OBJECT_ATTRIBUTES);
	attr.RootDirectory = NULL;
	attr.ObjectName = &object_name;
	attr.Attributes = 0;
	attr.SecurityDescriptor = NULL;
	attr.SecurityQualityOfService = NULL;
	IO_STATUS_BLOCK status_block;
	NTSTATUS status;
	HANDLE handle;
	status = NtOpenFile(&handle, DELETE, &attr, &status_block, FILE_SHARE_DELETE, FILE_NON_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT);
	if (!NT_SUCCESS(status))
	{
		if (status != STATUS_SHARING_VIOLATION)
		{
			log_warning("NtOpenFile() failed, status: %x\n", status);
			return -ENOENT;
		}
		/* This file has open handles in some processes, even we set delete disposition flags
		 * The actual deletion of the file will be delayed to the last handle closing
		 * To make the file disappear from its parent directory immediately, we move the file
		 * to Windows recycle bin prior to deletion.
		 */
		status = NtOpenFile(&handle, DELETE, &attr, &status_block, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			FILE_NON_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT);
		if (!NT_SUCCESS(status))
		{
			log_warning("NtOpenFile() failed, status: %x\n", status);
			return -EBUSY;
		}
		status = move_to_recycle_bin(handle, wpathname);
		if (!NT_SUCCESS(status))
			return -EBUSY;
	}
	/* Set disposition flag */
	FILE_DISPOSITION_INFORMATION info;
	info.DeleteFile = TRUE;
	status = NtSetInformationFile(handle, &status_block, &info, sizeof(info), FileDispositionInformation);
	if (!NT_SUCCESS(status))
	{
		log_warning("NtSetInformation(FileDispositionInformation) failed, status: %x\n", status);
		return -EBUSY;
	}
	NtClose(handle);
	return 0;
}

static int winfs_rename(struct file_system *fs, struct file *f, const char *newpath)
{
	AcquireSRWLockShared(&f->rw_lock);
	struct winfs_file *winfile = (struct winfs_file *)f;
	char buf[sizeof(FILE_RENAME_INFORMATION) + PATH_MAX * 2];
	NTSTATUS status;
	int r = 0;
	int retry_count = 5;
retry:
	if (--retry_count == 0)
	{
		r = -EPERM;
		goto out;
	}
	FILE_RENAME_INFORMATION *info = (FILE_RENAME_INFORMATION *)buf;
	info->ReplaceIfExists = TRUE;
	info->RootDirectory = NULL;
	info->FileNameLength = 2 * filename_to_nt_pathname(newpath, info->FileName, PATH_MAX);
	if (info->FileNameLength == 0)
	{
		r = -ENOENT;
		goto out;
	}
	IO_STATUS_BLOCK status_block;
	status = NtSetInformationFile(winfile->handle, &status_block, info, info->FileNameLength + sizeof(FILE_RENAME_INFORMATION), FileRenameInformation);
	if (!NT_SUCCESS(status))
	{
		if (status == STATUS_ACCESS_DENIED)
		{
			/* The destination exists and the operation cannot be completed via a native operation.
			 * We remove the destination file first, then move this file again.
			 */
			r = winfs_unlink(fs, newpath);
			if (r)
				goto out;
			goto retry;
		}
		log_warning("NtSetInformationFile() failed, status: %x\n", status);
		r = -ENOENT;
		goto out;
	}
out:
	ReleaseSRWLockShared(&f->rw_lock);
	return r;
}

static int winfs_mkdir(struct file_system *fs, const char *pathname, int mode)
{
	WCHAR wpathname[PATH_MAX];

	if (utf8_to_utf16_filename(pathname, strlen(pathname) + 1, wpathname, PATH_MAX) <= 0)
		return -ENOENT;
	if (!CreateDirectoryW(wpathname, NULL))
	{
		DWORD err = GetLastError();
		if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS)
		{
			log_warning("File already exists.\n");
			return -EEXIST;
		}
		log_warning("CreateDirectoryW() failed, error code: %d\n", GetLastError());
		return -ENOENT;
	}
	return 0;
}

static int winfs_rmdir(struct file_system *fs, const char *pathname)
{
	WCHAR wpathname[PATH_MAX];
	if (utf8_to_utf16_filename(pathname, strlen(pathname) + 1, wpathname, PATH_MAX) <= 0)
		return -ENOENT;
	if (!RemoveDirectoryW(wpathname))
	{
		log_warning("RemoveDirectoryW() failed, error code: %d\n", GetLastError());
		return -ENOENT;
	}
	return 0;
}

/* Open a file
 * Return values:
 *  < 0 => errno
 * == 0 => Opening file succeeded
 *  > 0 => It is a symlink which needs to be redirected (target written)
 */
static int open_file(HANDLE *hFile, const char *pathname, DWORD desired_access, DWORD create_disposition,
	int flags, BOOL bInherit, char *target, int buflen)
{
	WCHAR buf[PATH_MAX];
	UNICODE_STRING name;
	name.Buffer = buf;
	name.MaximumLength = name.Length = 2 * filename_to_nt_pathname(pathname, buf, PATH_MAX);
	if (name.Length == 0)
		return -ENOENT;

	OBJECT_ATTRIBUTES attr;
	attr.Length = sizeof(OBJECT_ATTRIBUTES);
	attr.RootDirectory = NULL;
	attr.ObjectName = &name;
	attr.Attributes = (bInherit? OBJ_INHERIT: 0);
	attr.SecurityDescriptor = NULL;
	attr.SecurityQualityOfService = NULL;

	NTSTATUS status;
	IO_STATUS_BLOCK status_block;
	HANDLE handle;
	DWORD create_options = FILE_SYNCHRONOUS_IO_NONALERT; /* For synchronous I/O */
	if (desired_access & GENERIC_ALL)
		create_options |= FILE_OPEN_FOR_BACKUP_INTENT | FILE_OPEN_REMOTE_INSTANCE;
	else
	{
		if (desired_access & GENERIC_READ)
			create_options |= FILE_OPEN_FOR_BACKUP_INTENT;
		if (desired_access & GENERIC_WRITE)
			create_options |= FILE_OPEN_REMOTE_INSTANCE;
	}
	desired_access |= SYNCHRONIZE | FILE_READ_ATTRIBUTES;
	status = NtCreateFile(&handle, desired_access, &attr, &status_block, NULL,
		FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		create_disposition, create_options, NULL, 0);
	if (status == STATUS_OBJECT_NAME_COLLISION)
	{
		log_warning("File already exists.\n");
		return -EEXIST;
	}
	else if (!NT_SUCCESS(status))
	{
		log_warning("Unhandled NtCreateFile error, status: %x, returning ENOENT.\n", status);
		return -ENOENT;
	}

	FILE_ATTRIBUTE_TAG_INFORMATION attribute_info;
	status = NtQueryInformationFile(handle, &status_block, &attribute_info, sizeof(attribute_info), FileAttributeTagInformation);
	if (!NT_SUCCESS(status))
	{
		log_error("NtQueryInformationFile(FileAttributeTagInformation) failed, status: %x\n", status);
		NtClose(handle);
		return -EIO;
	}
	/* Test if the file is a symlink */
	int is_symlink = 0;
	if (attribute_info.FileAttributes & FILE_ATTRIBUTE_SYSTEM)
	{
		/* The file has system flag set. A potential symbolic link. */
		if (!(desired_access & GENERIC_READ))
		{
			/* But the handle does not have READ access, try reopening file */
			HANDLE read_handle = ReOpenFile(handle, desired_access | GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_BACKUP_SEMANTICS);
			if (read_handle == INVALID_HANDLE_VALUE)
			{
				log_warning("Reopen symlink file failed, error code %d. Assume not symlink.\n", GetLastError());
				return 0;
			}
			CloseHandle(handle);
			handle = read_handle;
		}
		if (winfs_read_symlink(handle, target, buflen) > 0)
		{
			if (!(flags & O_NOFOLLOW))
			{
				CloseHandle(handle);
				return 1;
			}
			if (!(flags & O_PATH))
			{
				CloseHandle(handle);
				log_info("Specified O_NOFOLLOW but not O_PATH, returning ELOOP.\n");
				return -ELOOP;
			}
		}
	}
	else if (!(attribute_info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (flags & O_DIRECTORY))
	{
		log_warning("Not a directory.\n");
		return -ENOTDIR;
	}
	*hFile = handle;
	return 0;
}

static int winfs_open(struct file_system *fs, const char *pathname, int flags, int mode, struct file **fp, char *target, int buflen)
{
	/* TODO: mode */
	DWORD desired_access, create_disposition;
	HANDLE handle;

	if (flags & O_PATH)
		desired_access = 0;
	else if (flags & O_RDWR)
		desired_access = GENERIC_READ | GENERIC_WRITE;
	else if (flags & O_WRONLY)
		desired_access = GENERIC_WRITE;
	else
		desired_access = GENERIC_READ;
	if (flags & __O_DELETE)
		desired_access |= DELETE;
	if (flags & O_EXCL)
		create_disposition = FILE_CREATE;
	else if (flags & O_CREAT)
		create_disposition = FILE_OPEN_IF;
	else
		create_disposition = FILE_OPEN;
	int r = open_file(&handle, pathname, desired_access, create_disposition, flags, fp != NULL, target, buflen);
	if (r < 0 || r == 1)
		return r;
	if ((flags & O_TRUNC) && ((flags & O_WRONLY) || (flags & O_RDWR)))
	{
		/* Truncate the file */
		FILE_END_OF_FILE_INFORMATION info;
		info.EndOfFile.QuadPart = 0;
		IO_STATUS_BLOCK status_block;
		NTSTATUS status = NtSetInformationFile(handle, &status_block, &info, sizeof(info), FileEndOfFileInformation);
		if (!NT_SUCCESS(status))
			log_error("NtSetInformationFile() failed, status: %x\n", status);
	}

	if (fp)
	{
		int pathlen = strlen(pathname);
		struct winfs_file *file = (struct winfs_file *)kmalloc(sizeof(struct winfs_file) + pathlen);
		file_init(&file->base_file, &winfs_ops, flags);
		file->handle = handle;
		file->restart_scan = 1;
		file->pathlen = pathlen;
		memcpy(file->pathname, pathname, pathlen);
		*fp = (struct file *)file;
	}
	else
		CloseHandle(handle);
	return 0;
}

struct winfs
{
	struct file_system base_fs;
};

struct file_system *winfs_alloc()
{
	struct winfs *fs = (struct winfs *)kmalloc(sizeof(struct winfs));
	fs->base_fs.mountpoint = "/";
	fs->base_fs.open = winfs_open;
	fs->base_fs.symlink = winfs_symlink;
	fs->base_fs.link = winfs_link;
	fs->base_fs.unlink = winfs_unlink;
	fs->base_fs.rename = winfs_rename;
	fs->base_fs.mkdir = winfs_mkdir;
	fs->base_fs.rmdir = winfs_rmdir;
	return (struct file_system *)fs;
}

int winfs_is_winfile(struct file *f)
{
	return f->op_vtable == &winfs_ops;
}
