/** Implementation of the Large Objects interface.
 *
 * Allows direct access to large objects, as well as though I/O streams.
 *
 * Copyright (c) 2003-2017, Jeroen T. Vermeulen <jtv@xs4all.nl>
 *
 * See COPYING for copyright license.  If you did not receive a file called
 * COPYING with this source code, please notify the distributor of this mistake,
 * or contact the author.
 */
#include "pqxx/compiler-internal.hxx"

#include <algorithm>
#include <cerrno>
#include <stdexcept>

#include "libpq-fe.h"

//#include "libpq/libpq-fs.h"
/// Copied from libpq/libpq-fs.h so we don't need that header anymore
#define INV_WRITE		0x00020000
/// Copied from libpq/libpq-fs.h so we don't need that header anymore
#define INV_READ		0x00040000

#include "pqxx/largeobject"

#include "pqxx/internal/gates/connection-largeobject.hxx"


using namespace pqxx::internal;

namespace
{

inline int StdModeToPQMode(std::ios::openmode mode)
{
  return
	((mode & std::ios::in)  ? INV_READ  : 0) |
	((mode & std::ios::out) ? INV_WRITE : 0);
}


inline int StdDirToPQDir(std::ios::seekdir dir) noexcept
{
  // TODO: Figure out whether seekdir values match C counterparts!
#ifdef PQXX_SEEKDIRS_MATCH_C
  return dir;
#else
  int pqdir;
  switch (dir)
  {
  case std::ios::beg: pqdir=SEEK_SET; break;
  case std::ios::cur: pqdir=SEEK_CUR; break;
  case std::ios::end: pqdir=SEEK_END; break;

  /* Added mostly to silence compiler warning, but also to help compiler detect
   * cases where this function can be optimized away completely.  This latter
   * reason should go away as soon as PQXX_SEEKDIRS_MATCH_C works.
   */
  default: pqdir = dir; break;
  }
  return pqdir;
#endif
}


} // namespace


pqxx::largeobject::largeobject() noexcept :
  m_id(oid_none)
{
}


pqxx::largeobject::largeobject(dbtransaction &T) :
  m_id()
{
  m_id = lo_creat(raw_connection(T), INV_READ|INV_WRITE);
  if (m_id == oid_none)
  {
    const int err = errno;
    if (err == ENOMEM) throw std::bad_alloc();
    throw failure("Could not create large object: " + reason(err));
  }
}


pqxx::largeobject::largeobject(dbtransaction &T, const std::string &File) :
  m_id()
{
  m_id = lo_import(raw_connection(T), File.c_str());
  if (m_id == oid_none)
  {
    const int err = errno;
    if (err == ENOMEM) throw std::bad_alloc();
    throw failure(
	"Could not import file '" + File + "' to large object: " +
	reason(err));
  }
}


pqxx::largeobject::largeobject(const largeobjectaccess &O) noexcept :
  m_id(O.id())
{
}


void pqxx::largeobject::to_file(
	dbtransaction &T,
	const std::string &File) const
{
  if (lo_export(raw_connection(T), id(), File.c_str()) == -1)
  {
    const int err = errno;
    if (err == ENOMEM) throw std::bad_alloc();
    throw failure(
	"Could not export large object " + to_string(m_id) + " "
	"to file '" + File + "': " + reason(err));
  }
}


void pqxx::largeobject::remove(dbtransaction &T) const
{
  if (lo_unlink(raw_connection(T), id()) == -1)
  {
    const int err = errno;
    if (err == ENOMEM) throw std::bad_alloc();
    throw failure(
	"Could not delete large object " + to_string(m_id) + ": " +
	reason(err));
  }
}


pqxx::internal::pq::PGconn *pqxx::largeobject::raw_connection(
	const dbtransaction &T)
{
  return gate::connection_largeobject(T.conn()).raw_connection();
}


std::string pqxx::largeobject::reason(int err) const
{
  if (err == ENOMEM) return "Out of memory";
  if (id() == oid_none) return "No object selected";

  char buf[500];
  return std::string(strerror_wrapper(err, buf, sizeof(buf)));
}


pqxx::largeobjectaccess::largeobjectaccess(dbtransaction &T, openmode mode) :
  largeobject(T),
  m_trans(T),
  m_fd(-1)
{
  open(mode);
}


pqxx::largeobjectaccess::largeobjectaccess(
	dbtransaction &T,
	oid O,
	openmode mode) :
  largeobject(O),
  m_trans(T),
  m_fd(-1)
{
  open(mode);
}


pqxx::largeobjectaccess::largeobjectaccess(
	dbtransaction &T,
	largeobject O,
	openmode mode) :
  largeobject(O),
  m_trans(T),
  m_fd(-1)
{
  open(mode);
}


pqxx::largeobjectaccess::largeobjectaccess(
	dbtransaction &T,
	const std::string &File,
	openmode mode) :
  largeobject(T, File),
  m_trans(T),
  m_fd(-1)
{
  open(mode);
}


pqxx::largeobjectaccess::size_type
pqxx::largeobjectaccess::seek(size_type dest, seekdir dir)
{
  const auto Result = cseek(dest, dir);
  if (Result == -1)
  {
    const int err = errno;
    if (err == ENOMEM) throw std::bad_alloc();
    throw failure("Error seeking in large object: " + reason(err));
  }

  return Result;
}


pqxx::largeobjectaccess::pos_type
pqxx::largeobjectaccess::cseek(off_type dest, seekdir dir) noexcept
{
  return lo_lseek(raw_connection(), m_fd, int(dest), StdDirToPQDir(dir));
}


pqxx::largeobjectaccess::pos_type
pqxx::largeobjectaccess::cwrite(const char Buf[], size_type Len) noexcept
{
  return
    std::max(
	lo_write(raw_connection(), m_fd,const_cast<char *>(Buf), size_t(Len)),
        -1);
}


pqxx::largeobjectaccess::pos_type
pqxx::largeobjectaccess::cread(char Buf[], size_type Bytes) noexcept
{
  return std::max(lo_read(raw_connection(), m_fd, Buf, size_t(Bytes)), -1);
}


pqxx::largeobjectaccess::pos_type
pqxx::largeobjectaccess::ctell() const noexcept
{
  return lo_tell(raw_connection(), m_fd);
}


void pqxx::largeobjectaccess::write(const char Buf[], size_type Len)
{
  const auto Bytes = cwrite(Buf, Len);
  if (Bytes < Len)
  {
    const int err = errno;
    if (err == ENOMEM) throw std::bad_alloc();
    if (Bytes < 0)
      throw failure(
	"Error writing to large object #" + to_string(id()) + ": " +
	reason(err));
    if (Bytes == 0)
      throw failure(
	"Could not write to large object #" + to_string(id()) + ": " +
	reason(err));

    throw failure(
	"Wanted to write " + to_string(Len) + " bytes to large object #" +
	to_string(id()) + "; " "could only write " + to_string(Bytes));
  }
}


pqxx::largeobjectaccess::size_type
pqxx::largeobjectaccess::read(char Buf[], size_type Len)
{
  const auto Bytes = cread(Buf, Len);
  if (Bytes < 0)
  {
    const int err = errno;
    if (err == ENOMEM) throw std::bad_alloc();
    throw failure(
	"Error reading from large object #" + to_string(id()) + ": " +
	reason(err));
  }
  return Bytes;
}


void pqxx::largeobjectaccess::open(openmode mode)
{
  m_fd = lo_open(raw_connection(), id(), StdModeToPQMode(mode));
  if (m_fd < 0)
  {
    const int err = errno;
    if (err == ENOMEM) throw std::bad_alloc();
    throw failure(
	"Could not open large object " + to_string(id()) + ": " +
	reason(err));
  }
}


void pqxx::largeobjectaccess::close() noexcept
{
  if (m_fd >= 0) lo_close(raw_connection(), m_fd);
}


pqxx::largeobjectaccess::size_type pqxx::largeobjectaccess::tell() const
{
  const size_type res = ctell();
  if (res == -1) throw failure(reason(errno));
  return res;
}


std::string pqxx::largeobjectaccess::reason(int err) const
{
  return (m_fd == -1) ? "No object opened" : largeobject::reason(err);
}


void pqxx::largeobjectaccess::process_notice(const std::string &s) noexcept
{
  m_trans.process_notice(s);
}
