// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sql/statement.h"

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "third_party/sqlite/sqlite3.h"

namespace sql {

// This empty constructor initializes our reference with an empty one so that
// we don't have to NULL-check the ref_ to see if the statement is valid: we
// only have to check the ref's validity bit.
Statement::Statement()
    : ref_(new Connection::StatementRef(NULL, NULL, false)),
      stepped_(false),
      succeeded_(false) {
}

Statement::Statement(scoped_refptr<Connection::StatementRef> ref)
    : ref_(ref),
      stepped_(false),
      succeeded_(false) {
}

Statement::~Statement() {
  // Free the resources associated with this statement. We assume there's only
  // one statement active for a given sqlite3_stmt at any time, so this won't
  // mess with anything.
  Reset(true);
}

void Statement::Assign(scoped_refptr<Connection::StatementRef> ref) {
  Reset(true);
  ref_ = ref;
}

void Statement::Clear() {
  Assign(new Connection::StatementRef(NULL, NULL, false));
  succeeded_ = false;
}

bool Statement::CheckValid() const {
  // Allow operations to fail silently if a statement was invalidated
  // because the database was closed by an error handler.
  DLOG_IF(FATAL, !ref_->was_valid())
      << "Cannot call mutating statements on an invalid statement.";
  return is_valid();
}

bool Statement::Run() {
  DCHECK(!stepped_);
  ref_->AssertIOAllowed();
  if (!CheckValid())
    return false;

  stepped_ = true;
  return CheckError(sqlite3_step(ref_->stmt())) == SQLITE_DONE;
}

bool Statement::Step() {
  ref_->AssertIOAllowed();
  if (!CheckValid())
    return false;

  stepped_ = true;
  return CheckError(sqlite3_step(ref_->stmt())) == SQLITE_ROW;
}

void Statement::Reset(bool clear_bound_vars) {
  ref_->AssertIOAllowed();
  if (is_valid()) {
    // We don't call CheckError() here because sqlite3_reset() returns
    // the last error that Step() caused thereby generating a second
    // spurious error callback.
    if (clear_bound_vars)
      sqlite3_clear_bindings(ref_->stmt());
    sqlite3_reset(ref_->stmt());
  }

  succeeded_ = false;
  stepped_ = false;
}

bool Statement::Succeeded() const {
  if (!is_valid())
    return false;

  return succeeded_;
}

bool Statement::BindNull(int col) {
  DCHECK(!stepped_);
  if (!is_valid())
    return false;

  return CheckOk(sqlite3_bind_null(ref_->stmt(), col + 1));
}

bool Statement::BindBool(int col, bool val) {
  return BindInt(col, val ? 1 : 0);
}

bool Statement::BindInt(int col, int val) {
  DCHECK(!stepped_);
  if (!is_valid())
    return false;

  return CheckOk(sqlite3_bind_int(ref_->stmt(), col + 1, val));
}

bool Statement::BindInt64(int col, int64 val) {
  DCHECK(!stepped_);
  if (!is_valid())
    return false;

  return CheckOk(sqlite3_bind_int64(ref_->stmt(), col + 1, val));
}

bool Statement::BindDouble(int col, double val) {
  DCHECK(!stepped_);
  if (!is_valid())
    return false;

  return CheckOk(sqlite3_bind_double(ref_->stmt(), col + 1, val));
}

bool Statement::BindCString(int col, const char* val) {
  DCHECK(!stepped_);
  if (!is_valid())
    return false;

  return CheckOk(
      sqlite3_bind_text(ref_->stmt(), col + 1, val, -1, SQLITE_TRANSIENT));
}

bool Statement::BindString(int col, const std::string& val) {
  DCHECK(!stepped_);
  if (!is_valid())
    return false;

  return CheckOk(sqlite3_bind_text(ref_->stmt(),
                                   col + 1,
                                   val.data(),
                                   val.size(),
                                   SQLITE_TRANSIENT));
}

bool Statement::BindString16(int col, const string16& value) {
  return BindString(col, UTF16ToUTF8(value));
}

bool Statement::BindBlob(int col, const void* val, int val_len) {
  DCHECK(!stepped_);
  if (!is_valid())
    return false;

  return CheckOk(
      sqlite3_bind_blob(ref_->stmt(), col + 1, val, val_len, SQLITE_TRANSIENT));
}

int Statement::ColumnCount() const {
  if (!is_valid())
    return 0;

  return sqlite3_column_count(ref_->stmt());
}

ColType Statement::ColumnType(int col) const {
  // Verify that our enum matches sqlite's values.
  COMPILE_ASSERT(COLUMN_TYPE_INTEGER == SQLITE_INTEGER, integer_no_match);
  COMPILE_ASSERT(COLUMN_TYPE_FLOAT == SQLITE_FLOAT, float_no_match);
  COMPILE_ASSERT(COLUMN_TYPE_TEXT == SQLITE_TEXT, integer_no_match);
  COMPILE_ASSERT(COLUMN_TYPE_BLOB == SQLITE_BLOB, blob_no_match);
  COMPILE_ASSERT(COLUMN_TYPE_NULL == SQLITE_NULL, null_no_match);

  return static_cast<ColType>(sqlite3_column_type(ref_->stmt(), col));
}

ColType Statement::DeclaredColumnType(int col) const {
  std::string column_type(sqlite3_column_decltype(ref_->stmt(), col));
  StringToLowerASCII(&column_type);

  if (column_type == "integer")
    return COLUMN_TYPE_INTEGER;
  else if (column_type == "float")
    return COLUMN_TYPE_FLOAT;
  else if (column_type == "text")
    return COLUMN_TYPE_TEXT;
  else if (column_type == "blob")
    return COLUMN_TYPE_BLOB;

  return COLUMN_TYPE_NULL;
}

bool Statement::ColumnBool(int col) const {
  return !!ColumnInt(col);
}

int Statement::ColumnInt(int col) const {
  if (!CheckValid())
    return 0;

  return sqlite3_column_int(ref_->stmt(), col);
}

int64 Statement::ColumnInt64(int col) const {
  if (!CheckValid())
    return 0;

  return sqlite3_column_int64(ref_->stmt(), col);
}

double Statement::ColumnDouble(int col) const {
  if (!CheckValid())
    return 0;

  return sqlite3_column_double(ref_->stmt(), col);
}

std::string Statement::ColumnString(int col) const {
  if (!CheckValid())
    return std::string();

  const char* str = reinterpret_cast<const char*>(
      sqlite3_column_text(ref_->stmt(), col));
  int len = sqlite3_column_bytes(ref_->stmt(), col);

  std::string result;
  if (str && len > 0)
    result.assign(str, len);
  return result;
}

string16 Statement::ColumnString16(int col) const {
  if (!CheckValid())
    return string16();

  std::string s = ColumnString(col);
  return !s.empty() ? UTF8ToUTF16(s) : string16();
}

int Statement::ColumnByteLength(int col) const {
  if (!CheckValid())
    return 0;

  return sqlite3_column_bytes(ref_->stmt(), col);
}

const void* Statement::ColumnBlob(int col) const {
  if (!CheckValid())
    return NULL;

  return sqlite3_column_blob(ref_->stmt(), col);
}

bool Statement::ColumnBlobAsString(int col, std::string* blob) {
  if (!CheckValid())
    return false;

  const void* p = ColumnBlob(col);
  size_t len = ColumnByteLength(col);
  blob->resize(len);
  if (blob->size() != len) {
    return false;
  }
  blob->assign(reinterpret_cast<const char*>(p), len);
  return true;
}

bool Statement::ColumnBlobAsString16(int col, string16* val) const {
  if (!CheckValid())
    return false;

  const void* data = ColumnBlob(col);
  size_t len = ColumnByteLength(col) / sizeof(char16);
  val->resize(len);
  if (val->size() != len)
    return false;
  val->assign(reinterpret_cast<const char16*>(data), len);
  return true;
}

bool Statement::ColumnBlobAsVector(int col, std::vector<char>* val) const {
  val->clear();

  if (!CheckValid())
    return false;

  const void* data = sqlite3_column_blob(ref_->stmt(), col);
  int len = sqlite3_column_bytes(ref_->stmt(), col);
  if (data && len > 0) {
    val->resize(len);
    memcpy(&(*val)[0], data, len);
  }
  return true;
}

bool Statement::ColumnBlobAsVector(
    int col,
    std::vector<unsigned char>* val) const {
  return ColumnBlobAsVector(col, reinterpret_cast< std::vector<char>* >(val));
}

const char* Statement::GetSQLStatement() {
  return sqlite3_sql(ref_->stmt());
}

bool Statement::CheckOk(int err) const {
  // Binding to a non-existent variable is evidence of a serious error.
  // TODO(gbillock,shess): make this invalidate the statement so it
  // can't wreak havoc.
  if (err == SQLITE_RANGE)
    DLOG(FATAL) << "Bind value out of range";
  return err == SQLITE_OK;
}

int Statement::CheckError(int err) {
  // Please don't add DCHECKs here, OnSqliteError() already has them.
  succeeded_ = (err == SQLITE_OK || err == SQLITE_ROW || err == SQLITE_DONE);
  if (!succeeded_ && ref_.get() && ref_->connection())
    return ref_->connection()->OnSqliteError(err, this, NULL);
  return err;
}

}  // namespace sql
