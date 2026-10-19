# Modified by: gengxun + Codex
# Date: 2026-10-19
# Summary: Extract this generic renderer component from the mission repository.

"""Platform-native no-replace atomic publication primitives.

This module deliberately requires ``renameat2(RENAME_NOREPLACE)``.  It never
falls back to a check-then-rename sequence that could overwrite a competing
publisher's file or directory.

Modified by: gengxun + Codex
Date: 2026-08-09
Summary: Add attested publication with precomputed identity, safe rollback preservation, and FD/path ownership lifecycle.
Modified by: gengxun + Codex
Date: 2026-08-18
Summary: Add Win32 no-replace publication and handle-bound regular-file reads.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
import ctypes
from dataclasses import dataclass
import errno
import hashlib
import json
import os
from pathlib import Path
import secrets
import stat
import sys
import tempfile
from typing import TypeAlias


_AT_FDCWD = -100
_RENAME_NOREPLACE = 1

DirectoryIdentity: TypeAlias = tuple[int, int]
FileInodeIdentity: TypeAlias = tuple[int, int]


@dataclass(frozen=True)
class RegularFileIdentity:
    """Stable no-follow identity and content digest for one regular file."""

    device: int
    inode: int
    mode: int
    size: int
    mtime_ns: int
    ctime_ns: int
    sha256: str


@dataclass(frozen=True)
class PublicationIdentity:
    """Rename-stable inode and content identity known by the publisher."""

    device: int
    inode: int
    mode: int
    size: int
    sha256: str


@dataclass(frozen=True)
class AttestedFilePublication:
    """One no-replace publication bound to its publisher-known inode."""

    path: Path
    identity: PublicationIdentity


@dataclass(frozen=True)
class DirectoryTreeEntry:
    """No-follow identity and content digest for one tree entry."""

    relative_path: str
    kind: str
    device: int
    inode: int
    mode: int
    size: int
    mtime_ns: int
    ctime_ns: int
    sha256: str | None


@dataclass(frozen=True)
class DirectoryTreeIdentity:
    """Stable ordered snapshot of a directory tree."""

    root_identity: DirectoryIdentity
    entries: tuple[DirectoryTreeEntry, ...]


class PublishedDurabilityUnconfirmedError(OSError):
    """Publication is visible, but its parent-directory fsync failed."""

    def __init__(
        self,
        destination: Path,
        cause: OSError,
        publication: AttestedFilePublication | None = None,
    ):
        self.destination = Path(destination)
        self.cause = cause
        self.publication = publication
        self.publication_identity = (
            publication.identity if publication is not None else None
        )
        super().__init__(
            cause.errno,
            "publication is visible but parent-directory durability is unconfirmed: "
            f"{self.destination}: {cause}",
            str(self.destination),
        )


class PublicationRollbackConflictError(RuntimeError):
    """Rollback preserved an isolated object it could not restore or remove."""

    def __init__(
        self,
        destination: Path,
        preserved_path: Path,
        cause: BaseException,
    ) -> None:
        self.destination = Path(destination)
        self.preserved_path = Path(preserved_path)
        self.cause = cause
        super().__init__(
            "publication rollback could not safely restore or remove an isolated "
            f"object; it was preserved at {self.preserved_path}: {cause}"
        )


class PublicationRollbackError(RuntimeError):
    """Rollback changed visibility safely but could not confirm durability."""

    def __init__(self, destination: Path, cause: BaseException) -> None:
        self.destination = Path(destination)
        self.cause = cause
        super().__init__(f"publication rollback failed for {destination}: {cause}")


def rename_noreplace(source: Path, destination: Path) -> None:
    """Atomically rename ``source`` only if ``destination`` does not exist."""

    if sys.platform == "win32":
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        move_file = kernel32.MoveFileExW
        move_file.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32)
        move_file.restype = ctypes.c_int
        ctypes.set_last_error(0)
        if move_file(str(Path(source)), str(Path(destination)), 0x00000008):
            return
        error_number = ctypes.get_last_error()
        if error_number in {80, 183}:
            raise FileExistsError(
                errno.EEXIST,
                "destination already exists",
                str(destination),
            )
        if error_number in {2, 3}:
            raise FileNotFoundError(error_number, "source path is missing", str(source))
        raise OSError(error_number, "MoveFileExW failed", str(destination))
    if sys.platform != "linux":
        raise NotImplementedError(
            "atomic no-replace publication requires Linux renameat2"
        )
    try:
        renameat2 = ctypes.CDLL(None, use_errno=True).renameat2
    except AttributeError as error:
        raise NotImplementedError(
            "libc renameat2(RENAME_NOREPLACE) is unavailable"
        ) from error
    renameat2.argtypes = (
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    )
    renameat2.restype = ctypes.c_int
    source_bytes = os.fsencode(Path(source))
    destination_bytes = os.fsencode(Path(destination))
    ctypes.set_errno(0)
    result = renameat2(
        _AT_FDCWD,
        source_bytes,
        _AT_FDCWD,
        destination_bytes,
        _RENAME_NOREPLACE,
    )
    if result == 0:
        return
    error_number = ctypes.get_errno()
    if error_number == errno.EEXIST:
        raise FileExistsError(
            error_number,
            os.strerror(error_number),
            str(destination),
        )
    if error_number in {errno.ENOSYS, errno.EINVAL, errno.EOPNOTSUPP}:
        raise NotImplementedError(
            "filesystem does not support renameat2(RENAME_NOREPLACE)"
        )
    raise OSError(error_number, os.strerror(error_number), str(destination))


def _publication_identity(
    observed: os.stat_result,
    content: bytes,
) -> PublicationIdentity:
    if not stat.S_ISREG(observed.st_mode):
        raise ValueError("published object must remain a regular file")
    if observed.st_size != len(content):
        raise ValueError("published object size changed before attestation")
    return PublicationIdentity(
        device=observed.st_dev,
        inode=observed.st_ino,
        mode=observed.st_mode,
        size=observed.st_size,
        sha256=hashlib.sha256(content).hexdigest(),
    )


def _publication_from_regular_identity(
    path: Path,
    observed: RegularFileIdentity,
) -> AttestedFilePublication:
    return AttestedFilePublication(
        path=Path(path),
        identity=PublicationIdentity(
            device=observed.device,
            inode=observed.inode,
            mode=observed.mode,
            size=observed.size,
            sha256=observed.sha256,
        ),
    )


def _cleanup_temporary_if_owned(
    temporary: Path,
    expected_inode: FileInodeIdentity,
) -> None:
    try:
        observed = regular_file_identity(temporary)
    except FileNotFoundError:
        return
    if (observed.device, observed.inode) != expected_inode:
        return
    rollback_attested_publication(
        _publication_from_regular_identity(temporary, observed)
    )


def atomic_write_bytes_attested(
    destination: Path,
    content: bytes,
) -> AttestedFilePublication:
    """Publish bytes and return the inode/content identity known by the writer."""

    target = Path(destination)
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{target.name}.tmp-", dir=target.parent
    )
    temporary = Path(temporary_name)
    expected_inode: FileInodeIdentity | None = None
    descriptor_owned = True
    rename_completed = False
    primary_error: BaseException | None = None
    try:
        created = os.fstat(descriptor)
        if not stat.S_ISREG(created.st_mode):
            raise ValueError("atomic temporary must be a regular file")
        expected_inode = (created.st_dev, created.st_ino)
        with os.fdopen(descriptor, "wb", closefd=False) as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        publication = AttestedFilePublication(
            path=target,
            identity=_publication_identity(os.fstat(descriptor), content),
        )
        descriptor_owned = False
        os.close(descriptor)
        rename_noreplace(temporary, target)
        rename_completed = True
        try:
            _fsync_directory(target.parent)
        except OSError as error:
            raise PublishedDurabilityUnconfirmedError(
                target,
                error,
                publication,
            ) from error
    except BaseException as error:
        primary_error = error
        raise
    finally:
        finalizer_error = primary_error
        if descriptor_owned:
            descriptor_owned = False
            try:
                os.close(descriptor)
            except BaseException as close_error:
                if finalizer_error is None:
                    finalizer_error = close_error
                else:
                    finalizer_error.add_note(
                        "temporary descriptor close also failed: "
                        f"{type(close_error).__name__}: {close_error}"
                    )
        if not rename_completed and expected_inode is not None:
            try:
                _cleanup_temporary_if_owned(temporary, expected_inode)
            except BaseException as cleanup_error:
                if finalizer_error is None:
                    finalizer_error = cleanup_error
                else:
                    finalizer_error.add_note(
                        "owned temporary cleanup also failed: "
                        f"{type(cleanup_error).__name__}: {cleanup_error}"
                    )
        if primary_error is None and finalizer_error is not None:
            raise finalizer_error
    return publication


def atomic_write_bytes(destination: Path, content: bytes) -> Path:
    """Durably write and publish a new file without replacing a competitor."""

    return atomic_write_bytes_attested(Path(destination), content).path


def atomic_write_text(destination: Path, text: str) -> Path:
    """Encode ASCII/LF text and publish it with no-replace semantics."""

    return atomic_write_bytes(Path(destination), text.encode("ascii"))


def atomic_write_lines(destination: Path, lines: Iterable[str]) -> Path:
    """Durably publish ASCII LF-terminated lines without replacement."""

    target = Path(destination)
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{target.name}.tmp-", dir=target.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            for line in lines:
                if not isinstance(line, str):
                    raise TypeError("atomic output lines must be strings")
                if not line.endswith("\n") or "\r" in line:
                    raise ValueError("atomic output lines must be LF-terminated")
                stream.write(line.encode("ascii"))
            stream.flush()
            os.fsync(stream.fileno())
        rename_noreplace(temporary, target)
        try:
            _fsync_directory(target.parent)
        except OSError as error:
            raise PublishedDurabilityUnconfirmedError(target, error) from error
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
    return target


def _validated_json_bytes(document: Mapping[str, object]) -> bytes:
    if not isinstance(document, Mapping):
        raise ValueError("JSON root must be an object")
    encoded = (
        json.dumps(
            document,
            indent=2,
            sort_keys=True,
            ensure_ascii=True,
            allow_nan=False,
        ).encode("ascii")
        + b"\n"
    )
    loaded = json.loads(encoded)
    if not isinstance(loaded, dict):
        raise ValueError("JSON root must be an object")
    return encoded


def atomic_write_json(destination: Path, document: Mapping[str, object]) -> Path:
    """Validate and durably publish a new deterministic JSON object."""

    return atomic_write_bytes(Path(destination), _validated_json_bytes(document))


def atomic_write_json_attested(
    destination: Path,
    document: Mapping[str, object],
) -> AttestedFilePublication:
    """Validate and publish deterministic JSON with a writer-known identity."""

    return atomic_write_bytes_attested(
        Path(destination),
        _validated_json_bytes(document),
    )


def _matches_publication_identity(
    observed: RegularFileIdentity,
    expected: PublicationIdentity,
) -> bool:
    return (
        observed.device == expected.device
        and observed.inode == expected.inode
        and observed.mode == expected.mode
        and observed.size == expected.size
        and observed.sha256 == expected.sha256
    )


def _isolate_for_rollback(destination: Path) -> Path | None:
    for _ in range(128):
        isolated = destination.with_name(
            f".{destination.name}.rollback-{secrets.token_hex(16)}"
        )
        try:
            rename_noreplace(destination, isolated)
        except FileExistsError:
            continue
        except FileNotFoundError:
            return None
        return isolated
    raise FileExistsError(
        errno.EEXIST,
        "could not reserve a unique no-replace rollback path",
        str(destination),
    )


def _preserve_isolated_object(
    isolated: Path,
    destination: Path,
    preservation_stem: str,
) -> Path:
    visible_destination_name = destination.name.lstrip(".") or "publication"
    prefix = f"{visible_destination_name}.{preservation_stem}"
    suffix = 0
    while True:
        preserved = destination.with_name(
            prefix if suffix == 0 else f"{prefix}-{suffix}"
        )
        try:
            rename_noreplace(isolated, preserved)
        except FileExistsError:
            suffix += 1
            continue
        return preserved


def rollback_attested_publication(publication: AttestedFilePublication) -> None:
    """Remove only the attested inode, preserving every competing publication.

    The destination is first moved with Linux no-replace semantics.  Ownership
    is decided only after that directory entry has been atomically isolated;
    neither a digest match nor a check of the live destination can authorize
    deletion.
    """

    destination = Path(publication.path)
    isolated = _isolate_for_rollback(destination)
    if isolated is None:
        return

    try:
        observed = regular_file_identity(isolated)
    except BaseException as identity_error:
        try:
            rename_noreplace(isolated, destination)
        except BaseException as restore_error:
            preserved = _preserve_isolated_object(
                isolated,
                destination,
                f"unidentified-preserved-{secrets.token_hex(16)}",
            )
            restore_error.add_note(
                "isolated publication identity could not be read: "
                f"{type(identity_error).__name__}: {identity_error}"
            )
            try:
                _fsync_directory(destination.parent)
            except OSError as durability_error:
                restore_error.add_note(
                    "isolated-object preservation durability is unconfirmed: "
                    f"{durability_error}"
                )
            raise PublicationRollbackConflictError(
                destination,
                preserved,
                restore_error,
            ) from restore_error
        try:
            _fsync_directory(destination.parent)
        except OSError as durability_error:
            identity_error.add_note(
                "unidentifiable publication was restored but durability is "
                f"unconfirmed: {durability_error}"
            )
        raise

    if not _matches_publication_identity(observed, publication.identity):
        try:
            rename_noreplace(isolated, destination)
        except BaseException as restore_error:
            preserved = _preserve_isolated_object(
                isolated,
                destination,
                f"competitor-preserved-{observed.device}-{observed.inode}",
            )
            try:
                _fsync_directory(destination.parent)
            except OSError as durability_error:
                restore_error.add_note(
                    "competitor preservation durability is unconfirmed: "
                    f"{durability_error}"
                )
            raise PublicationRollbackConflictError(
                destination,
                preserved,
                restore_error,
            ) from restore_error
        try:
            _fsync_directory(destination.parent)
        except OSError as error:
            raise PublicationRollbackError(destination, error) from error
        return

    try:
        os.unlink(isolated)
    except FileNotFoundError:
        pass
    except OSError as first_cleanup_error:
        # A transient cleanup failure must not strand an accepted report under
        # the rollback name.  The second unlink remains identity-safe because
        # the isolated path has not been made available to another publisher.
        try:
            os.unlink(isolated)
        except FileNotFoundError:
            pass
        except OSError as cleanup_error:
            cleanup_error.add_note(
                "first isolated-publication cleanup attempt failed: "
                f"{first_cleanup_error}"
            )
            preserved = _preserve_isolated_object(
                isolated,
                destination,
                "publication-preserved-"
                f"{publication.identity.device}-{publication.identity.inode}",
            )
            try:
                _fsync_directory(destination.parent)
            except OSError as durability_error:
                cleanup_error.add_note(
                    "isolated-object preservation durability is unconfirmed: "
                    f"{durability_error}"
                )
            raise PublicationRollbackConflictError(
                destination,
                preserved,
                cleanup_error,
            ) from cleanup_error
    try:
        _fsync_directory(destination.parent)
    except OSError as error:
        raise PublicationRollbackError(destination, error) from error


def atomic_replace_bytes(destination: Path, content: bytes) -> Path:
    """Durably replace one explicitly mutable file with complete bytes."""

    target = Path(destination)
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{target.name}.tmp-", dir=target.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, target)
        try:
            _fsync_directory(target.parent)
        except OSError as error:
            raise PublishedDurabilityUnconfirmedError(target, error) from error
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
    return target


def atomic_replace_json(
    destination: Path, document: Mapping[str, object]
) -> Path:
    """Validate and durably replace one explicitly mutable JSON object."""

    return atomic_replace_bytes(Path(destination), _validated_json_bytes(document))


def directory_identity(path: Path) -> DirectoryIdentity:
    """Return the no-follow device/inode identity of a real directory."""

    candidate = Path(path)
    observed = os.lstat(candidate)
    if not stat.S_ISDIR(observed.st_mode) or stat.S_ISLNK(observed.st_mode):
        raise ValueError(
            f"publication source must be a non-symlink directory: {candidate}"
        )
    return (observed.st_dev, observed.st_ino)


def require_directory_identity(
    path: Path,
    expected_identity: DirectoryIdentity,
) -> None:
    """Reject a missing, replaced, or symlinked directory path."""

    observed_identity = directory_identity(path)
    if observed_identity != expected_identity:
        raise ValueError(
            "directory identity changed: "
            f"{Path(path)}; expected={expected_identity}; "
            f"observed={observed_identity}"
        )


def _open_regular_file_nofollow_nonblocking(
    path: Path,
) -> tuple[int, os.stat_result]:
    if sys.platform == "win32":
        import msvcrt

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        create_file = kernel32.CreateFileW
        create_file.argtypes = (
            ctypes.c_wchar_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
        )
        create_file.restype = ctypes.c_void_p
        get_attributes = kernel32.GetFileInformationByHandleEx
        get_attributes.argtypes = (
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_uint32,
        )
        get_attributes.restype = ctypes.c_int

        class _AttributeTagInfo(ctypes.Structure):
            _fields_ = [
                ("file_attributes", ctypes.c_uint32),
                ("reparse_tag", ctypes.c_uint32),
            ]

        invalid_handle = ctypes.c_void_p(-1).value
        ctypes.set_last_error(0)
        handle = create_file(
            str(Path(path)),
            0x80000000,
            0x00000001 | 0x00000002 | 0x00000004,
            None,
            3,
            0x00200000 | 0x08000000,
            None,
        )
        if handle in {None, invalid_handle}:
            error_number = ctypes.get_last_error()
            if error_number in {2, 3}:
                raise FileNotFoundError(error_number, "file is missing", str(path))
            raise OSError(error_number, "cannot open regular file", str(path))
        attributes = _AttributeTagInfo()
        if not get_attributes(handle, 9, ctypes.byref(attributes), ctypes.sizeof(attributes)):
            error_number = ctypes.get_last_error()
            kernel32.CloseHandle(handle)
            raise OSError(error_number, "cannot inspect regular file", str(path))
        if attributes.file_attributes & (0x00000010 | 0x00000400):
            kernel32.CloseHandle(handle)
            raise ValueError(f"publication tree entry must be a regular non-reparse file: {path}")
        try:
            descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
        except BaseException:
            kernel32.CloseHandle(handle)
            raise
        observed = os.fstat(descriptor)
        if not stat.S_ISREG(observed.st_mode):
            os.close(descriptor)
            raise ValueError(f"publication tree entry must be a regular file: {path}")
        return descriptor, observed
    try:
        nofollow = os.O_NOFOLLOW
    except AttributeError as error:
        raise NotImplementedError(
            "directory tree operations require platform O_NOFOLLOW support"
        ) from error
    descriptor = os.open(
        path,
        os.O_RDONLY | nofollow | os.O_NONBLOCK,
    )
    try:
        observed = os.fstat(descriptor)
        if not stat.S_ISREG(observed.st_mode):
            raise ValueError(
                f"publication tree entry must be a regular file: {path}"
            )
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor, observed


def _read_regular_file(
    path: Path,
    *,
    capture_content: bool,
) -> tuple[bytes | None, RegularFileIdentity]:
    descriptor, before = _open_regular_file_nofollow_nonblocking(path)
    content = bytearray() if capture_content else None
    try:
        digest = hashlib.sha256()
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            if content is not None:
                content.extend(chunk)
        after = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    before_identity = (
        before.st_dev,
        before.st_ino,
        before.st_mode,
        before.st_size,
        before.st_mtime_ns,
        before.st_ctime_ns,
    )
    after_identity = (
        after.st_dev,
        after.st_ino,
        after.st_mode,
        after.st_size,
        after.st_mtime_ns,
        after.st_ctime_ns,
    )
    if before_identity != after_identity:
        raise ValueError(f"regular file changed while reading: {path}")
    identity = RegularFileIdentity(
        device=after.st_dev,
        inode=after.st_ino,
        mode=after.st_mode,
        size=after.st_size,
        mtime_ns=after.st_mtime_ns,
        ctime_ns=after.st_ctime_ns,
        sha256=digest.hexdigest(),
    )
    return (bytes(content) if content is not None else None), identity


def regular_file_identity(path: Path) -> RegularFileIdentity:
    """Hash a stable no-follow regular file and return its identity."""

    _, identity = _read_regular_file(Path(path), capture_content=False)
    return identity


def read_regular_file_bytes(
    path: Path,
) -> tuple[bytes, RegularFileIdentity]:
    """Read and hash the same stable no-follow regular-file bytes."""

    content, identity = _read_regular_file(Path(path), capture_content=True)
    if content is None:
        raise AssertionError("captured regular-file content is unavailable")
    return content, identity


def _regular_file_tree_entry(path: Path, relative_path: str) -> DirectoryTreeEntry:
    identity = regular_file_identity(path)
    return DirectoryTreeEntry(
        relative_path=relative_path,
        kind="file",
        device=identity.device,
        inode=identity.inode,
        mode=identity.mode,
        size=identity.size,
        mtime_ns=identity.mtime_ns,
        ctime_ns=identity.ctime_ns,
        sha256=identity.sha256,
    )


def directory_tree_identity(root: Path) -> DirectoryTreeIdentity:
    """Snapshot a symlink-free directory tree with file content hashes."""

    candidate = Path(root)
    root_identity = directory_identity(candidate)
    entries: list[DirectoryTreeEntry] = []
    for directory, subdirectories, filenames in os.walk(
        candidate,
        topdown=True,
        onerror=_raise_walk_error,
    ):
        subdirectories.sort()
        filenames.sort()
        current = Path(directory)
        observed_directory = os.lstat(current)
        if not stat.S_ISDIR(observed_directory.st_mode) or stat.S_ISLNK(
            observed_directory.st_mode
        ):
            raise ValueError(
                f"publication tree entry must be a non-symlink directory: {current}"
            )
        relative_directory = current.relative_to(candidate)
        entries.append(
            DirectoryTreeEntry(
                relative_path=(
                    "."
                    if not relative_directory.parts
                    else relative_directory.as_posix()
                ),
                kind="directory",
                device=observed_directory.st_dev,
                inode=observed_directory.st_ino,
                mode=observed_directory.st_mode,
                # Directory size and timestamps are filesystem-dependent and
                # the root ctime may change solely because this directory is
                # renamed.  Entry names plus directory device/inode/mode bind
                # ownership and structure without treating publication itself
                # as a tree mutation.
                size=0,
                mtime_ns=0,
                ctime_ns=0,
                sha256=None,
            )
        )
        for subdirectory in subdirectories:
            path = current / subdirectory
            if path.is_symlink():
                raise ValueError(
                    f"publication tree must not contain symlinks: {path}"
                )
        for filename in filenames:
            path = current / filename
            if path.is_symlink():
                raise ValueError(
                    f"publication tree must not contain symlinks: {path}"
                )
            entries.append(
                _regular_file_tree_entry(
                    path,
                    path.relative_to(candidate).as_posix(),
                )
            )
    require_directory_identity(candidate, root_identity)
    return DirectoryTreeIdentity(
        root_identity=root_identity,
        entries=tuple(
            sorted(
                entries,
                key=lambda entry: (entry.relative_path, entry.kind),
            )
        ),
    )


def require_directory_tree_identity(
    path: Path,
    expected_identity: DirectoryTreeIdentity,
) -> None:
    """Reject a changed root, entry set, metadata, or file content."""

    observed_identity = directory_tree_identity(path)
    if observed_identity != expected_identity:
        raise ValueError(
            "directory tree identity changed: "
            f"{Path(path)}"
        )


def _rollback_unexpected_directory_rename(
    *,
    source: Path,
    destination: Path,
    unexpected_identity: DirectoryIdentity,
    error: BaseException,
) -> None:
    """Best-effort restore a mismatched destination to its source name."""

    try:
        require_directory_identity(destination, unexpected_identity)
        rename_noreplace(destination, source)
        require_directory_identity(source, unexpected_identity)
        if os.path.lexists(destination):
            raise RuntimeError(
                f"unexpected destination still exists after rollback: {destination}"
            )
        _fsync_directory(destination.parent)
    except BaseException as rollback_error:
        error.add_note(
            "could not roll back a directory whose identity changed during rename: "
            f"{type(rollback_error).__name__}: {rollback_error}"
        )


def publish_directory_noreplace(
    staged: Path,
    destination: Path,
    *,
    expected_identity: DirectoryIdentity | None = None,
    expected_tree_identity: DirectoryTreeIdentity | None = None,
) -> None:
    """Durably publish a staged directory without replacing any destination."""

    source = Path(staged)
    target = Path(destination)
    source_identity = directory_identity(source)
    identity = source_identity if expected_identity is None else expected_identity
    require_directory_identity(source, identity)
    if expected_tree_identity is not None:
        if expected_tree_identity.root_identity != identity:
            raise ValueError(
                "expected tree identity does not match expected root identity"
            )
        require_directory_tree_identity(source, expected_tree_identity)
    if source.parent.resolve() != target.parent.resolve():
        raise ValueError("staged and destination directories must share a parent")
    _fsync_tree(source)
    require_directory_identity(source, identity)
    if expected_tree_identity is not None:
        require_directory_tree_identity(source, expected_tree_identity)
    rename_noreplace(source, target)
    try:
        require_directory_identity(target, identity)
    except (OSError, ValueError) as identity_error:
        try:
            unexpected_identity = directory_identity(target)
        except (OSError, ValueError) as observation_error:
            identity_error.add_note(
                "could not identify the renamed destination for rollback: "
                f"{observation_error}"
            )
        else:
            _rollback_unexpected_directory_rename(
                source=source,
                destination=target,
                unexpected_identity=unexpected_identity,
                error=identity_error,
            )
        raise
    if expected_tree_identity is not None:
        try:
            require_directory_tree_identity(target, expected_tree_identity)
        except (OSError, ValueError) as tree_error:
            try:
                unexpected_identity = directory_identity(target)
            except (OSError, ValueError) as observation_error:
                tree_error.add_note(
                    "could not identify the renamed tree for rollback: "
                    f"{observation_error}"
                )
            else:
                _rollback_unexpected_directory_rename(
                    source=source,
                    destination=target,
                    unexpected_identity=unexpected_identity,
                    error=tree_error,
                )
            raise
    try:
        _fsync_directory(target.parent)
    except OSError as error:
        raise PublishedDurabilityUnconfirmedError(target, error) from error


def _fsync_tree(root: Path) -> None:
    directory_identity(root)
    for directory, subdirectories, filenames in os.walk(
        root,
        topdown=False,
        onerror=_raise_walk_error,
    ):
        current = Path(directory)
        for filename in filenames:
            path = current / filename
            if sys.platform == "win32":
                _flush_regular_file_windows(path)
                continue
            descriptor, _ = _open_regular_file_nofollow_nonblocking(path)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        for subdirectory in subdirectories:
            path = current / subdirectory
            if path.is_symlink():
                raise ValueError(f"staged publication must not contain symlinks: {path}")
        _fsync_directory(current)


def _flush_regular_file_windows(path: Path) -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    create_file = kernel32.CreateFileW
    create_file.argtypes = (
        ctypes.c_wchar_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
    )
    create_file.restype = ctypes.c_void_p
    invalid_handle = ctypes.c_void_p(-1).value
    handle = create_file(
        str(Path(path)),
        0x40000000,
        0x00000001 | 0x00000002 | 0x00000004,
        None,
        3,
        0x00200000,
        None,
    )
    if handle in {None, invalid_handle}:
        error_number = ctypes.get_last_error()
        raise OSError(error_number, "cannot open staged file for flush", str(path))
    try:
        if not kernel32.FlushFileBuffers(handle):
            error_number = ctypes.get_last_error()
            raise OSError(error_number, "cannot flush staged file", str(path))
    finally:
        kernel32.CloseHandle(handle)


def _raise_walk_error(error: OSError) -> None:
    raise error


def _fsync_directory(path: Path) -> None:
    if sys.platform == "win32":
        # Win32 publication uses MoveFileExW(MOVEFILE_WRITE_THROUGH). Windows
        # does not provide a portable directory-fsync descriptor equivalent.
        return
    flags = os.O_RDONLY | os.O_DIRECTORY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
