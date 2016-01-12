#include "MySQL.h"

#include <cassert>

#include <mysql/errmsg.h>

using namespace std;

MySQL::MySQL(bool _use_utf8) : use_utf8(_use_utf8)
{
  
}

MySQL::~MySQL() {
  if (conn) mysql_close(conn);
}

void
MySQL::begin() {
  mysql_autocommit(conn, 0); // disable autocommit
}

void
MySQL::commit() {
  if (mysql_commit(conn) != 0) {
    mysql_autocommit(conn, 1); // enable autocommit
    throw SQLException(SQLException::COMMIT_FAILED);
  } else {
    mysql_autocommit(conn, 1); // enable autocommit
  }
}

void
MySQL::rollback() {
  if (mysql_rollback(conn) != 0) {
    mysql_autocommit(conn, 1); // enable autocommit
    throw SQLException(SQLException::ROLLBACK_FAILED);
  } else {
    mysql_autocommit(conn, 1); // enable autocommit
  }
}

std::shared_ptr<SQLStatement>
MySQL::prepare(const std::string & query) {
  if (!conn) {
    throw SQLException(SQLException::PREPARE_FAILED, "Not connected", query);
  }
  MYSQL_STMT * stmt = 0;
  while ( 1 ) {
    if (stmt) mysql_stmt_close(stmt);	
    stmt = mysql_stmt_init(conn);
    if (!stmt) {
      if (mysql_errno(conn) == 2006) {
	continue;
      }
      throw SQLException(SQLException::PREPARE_FAILED, mysql_error(conn), query);
    }
    if (mysql_stmt_prepare(stmt, query.c_str(), query.size()) != 0) {
      if (mysql_errno(conn) == 2006) {
	continue;
      }
      throw SQLException(SQLException::PREPARE_FAILED, mysql_error(conn), query);
    }
    break;
  }
  return std::make_shared<MySQLStatement>(stmt, query);
}

bool
MySQL::connect(const string & _host_name, int _port, const string & _user_name, const string & _password, const string & _db_name) {
  host_name = _host_name;
  port = _port;
  user_name = _user_name;
  password = _password;
  db_name = _db_name;
  
  return connect();
}

bool
MySQL::connect() {
  if (conn) mysql_close(conn);
  conn = mysql_init(NULL);
  if (!conn) {
    return false;
  }

  int flags = CLIENT_FOUND_ROWS; 

  if (!mysql_real_connect(conn, host_name.c_str(), user_name.c_str(), password.c_str(), db_name.c_str(), port, 0, flags)) {
    const char * errmsg = mysql_error(conn);
    mysql_close(conn);
    conn = 0;
    return false;
  }
  
  if (use_utf8) {
    execute("SET NAMES utf8mb4");
  }
  
  return true;
}

bool
MySQL::ping() {
  if (conn && mysql_ping(conn) == 0) {
    return true;
  } else {
    return false;
  }
}

unsigned int
MySQL::execute(const char * query) {
  if (mysql_query(conn, query) != 0) {
    throw SQLException(SQLException::EXECUTE_FAILED, mysql_error(conn), query);
  }
  return mysql_affected_rows(conn);
}

MySQLStatement::MySQLStatement(MYSQL_STMT * _stmt, const std::string & _query)
  : SQLStatement(_query),
    stmt(_stmt)
{
  assert(stmt);
  num_bound_variables = mysql_stmt_param_count(stmt);

  for (unsigned int i = 0; i < MYSQL_MAX_BOUND_VARIABLES; i++) {
    bind_ptr[i] = 0;
  }

  reset();
}

MySQLStatement::~MySQLStatement() {
  if (stmt) {
    mysql_stmt_free_result(stmt);
    mysql_stmt_close(stmt);
  }
  for (unsigned int i = 0; i < MYSQL_MAX_BOUND_VARIABLES; i++) {
    char * ptr = bind_ptr[i];
    if (ptr) delete[] ptr;
  }
}

unsigned int
MySQLStatement::execute() {
  is_query_executed = true;
  has_result_set = false;
  
  if (mysql_stmt_bind_param(stmt, bind_data) != 0) {
    throw SQLException(SQLException::EXECUTE_FAILED, mysql_stmt_error(stmt), getQuery());
  }
  
  // Fetch result set meta information
  MYSQL_RES * prepare_meta_result = mysql_stmt_result_metadata(stmt);
  
  if (mysql_stmt_execute(stmt) != 0) {
    throw SQLException(SQLException::EXECUTE_FAILED, mysql_stmt_error(stmt), getQuery());
  }
  
  rows_affected = mysql_stmt_affected_rows(stmt);
  last_insert_id = mysql_stmt_insert_id(stmt);
  
  cerr << "rows affected = " << rows_affected << ", last_insert_id = " << last_insert_id << "\n";
  
  // NULL if no metadata / results
  if (prepare_meta_result) {
    // Get total columns in the query
    num_bound_variables = mysql_num_fields(prepare_meta_result);
    mysql_free_result(prepare_meta_result);

    // reserve varibles if larger than MAX
    memset(bind_data, 0, num_bound_variables * sizeof(MYSQL_BIND));
    
    for (unsigned int i = 0; i < num_bound_variables; i++) {
      bind_length[i] = 0;
      
      // bind[i].buffer_type = MYSQL_TYPE_STRING;
      bind_data[i].buffer = 0; // new char[255];
      bind_data[i].buffer_length = 0; // 255;
      bind_data[i].is_null = &bind_is_null[i];
      bind_data[i].length = &bind_length[i];
      bind_data[i].error = &bind_error[i];
    }
    
    /* Bind the result buffers */
    if (mysql_stmt_bind_result(stmt, bind_data) || mysql_stmt_store_result(stmt)) {
      for (unsigned int i = 0; i < num_bound_variables; i++) {
	if (bind_ptr[i]) {
	  delete[] bind_ptr[i];
	  bind_ptr[i] = 0;
	}
      }
      throw SQLException(SQLException::EXECUTE_FAILED, mysql_stmt_error(stmt), getQuery());
    }
    
    for (unsigned int i = 0; i < num_bound_variables; i++) {
      if (bind_ptr[i]) {
	delete[] bind_ptr[i];
	bind_ptr[i] = 0;
      }
    }
    
    has_result_set = true;
  }

  return rows_affected;
}

void
MySQLStatement::reset() {
  SQLStatement::reset();
  
  results_available = false;
  rows_affected = 0;
  is_query_executed = false;
  has_result_set = false;
  
  memset(bind_data, 0, num_bound_variables * sizeof(MYSQL_BIND));
  for (unsigned int i = 0; i < num_bound_variables; i++) {
    bindNull(i + 1);
  } 

  // no need to reset. just rebind parameter and execute again. not tested though
}

bool
MySQLStatement::next() {
  assert(stmt);
  
  results_available = false;
  rows_affected = 0;
  
  if (!is_query_executed) {
    execute();    
  }
  
  if (has_result_set) {
    int r = mysql_stmt_fetch(stmt);
        
    if (r == 0) {
      results_available = true;
    } else if (r == MYSQL_NO_DATA) {
    } else if (r == MYSQL_DATA_TRUNCATED) {
      results_available = true;
    } else if (r) {
      throw SQLException(SQLException::EXECUTE_FAILED, mysql_stmt_error(stmt), getQuery());
    }
  }
  
  return results_available;
}

void
MySQLStatement::bindNull(int index) {
  int a = 0;
  bindData(index, MYSQL_TYPE_LONG, &a, sizeof(a), true);
}

void
MySQLStatement::bind(int index, int value, bool is_defined) {
  if (!is_defined) value = 0;
  bindData(index, MYSQL_TYPE_LONG, &value, sizeof(value), is_defined);
}

void
MySQLStatement::bind(int index, long long value, bool is_defined) {
  if (!is_defined) value = 0;
  bindData(index, MYSQL_TYPE_LONGLONG, &value, sizeof(value), is_defined);
}

void
MySQLStatement::bind(int index, unsigned int value, bool is_defined) {
  if (!is_defined) value = 0;
  unsigned int v = value;
  bindData(index, MYSQL_TYPE_LONG, &v, sizeof(v), is_defined, true);
}

void
MySQLStatement::bind(int index, time_t value, bool is_defined) {
  int v = is_defined ? (int)value : 0;
  bindData(index, MYSQL_TYPE_LONG, &v, sizeof(v), is_defined);
}

void
MySQLStatement::bind(int index, double value, bool is_defined) {
  bindData(index, MYSQL_TYPE_DOUBLE, &value, sizeof(value), is_defined);
}

void
MySQLStatement::bind(int index, const char * value, bool is_defined) {
  bindData(index, MYSQL_TYPE_STRING, value, strlen(value), is_defined);
}

void
MySQLStatement::bind(int index, bool value, bool is_defined) {
  int a = value ? 1 : 0;
  bindData(index, MYSQL_TYPE_LONG, &a, sizeof(int), is_defined);
}

void
MySQLStatement::bind(int index, const std::string & value, bool is_defined) {
  bindData(index, MYSQL_TYPE_STRING, value.c_str(), value.size(), is_defined);
}

void
MySQLStatement::bind(int index, const ustring & value, bool is_defined) {
  bindData(index, MYSQL_TYPE_BLOB, value.data(), value.size(), is_defined);
}

void
MySQLStatement::bind(int index, const void * data, size_t len, bool is_defined) {
  bindData(index, MYSQL_TYPE_BLOB, data, len, is_defined);
}

int
MySQLStatement::getInt(int column_index) {
  if (column_index < 0 || column_index >= MYSQL_MAX_BOUND_VARIABLES) throw SQLException(SQLException::BAD_COLUMN_INDEX, "", getQuery());

  assert(stmt);
  
  long a = 0;
   
  if (bind_is_null[column_index]) {

  } else if (bind_length[column_index]) {
    long unsigned int dummy1;
    my_bool dummy2;
    MYSQL_BIND b;
    memset(&b, 0, sizeof(MYSQL_BIND));
    b.buffer_type = MYSQL_TYPE_LONG;
    b.buffer = (char *)&a;  
    b.buffer_length = sizeof(a);
    b.length = &dummy1; 
    b.is_null = &dummy2;
    if (mysql_stmt_fetch_column(stmt, &b, column_index, 0) != 0) {
      throw SQLException(SQLException::GET_FAILED, mysql_stmt_error(stmt), getQuery());
    }
  }

  return a;
}

unsigned int
MySQLStatement::getUInt(int column_index) {
  if (column_index < 0 || column_index >= MYSQL_MAX_BOUND_VARIABLES) throw SQLException(SQLException::BAD_COLUMN_INDEX, "", getQuery());
  assert(stmt);
  unsigned long a = 0;
  if (bind_is_null[column_index]) {
  } else if (bind_length[column_index]) {
    long unsigned int dummy1;
    my_bool dummy2;
    MYSQL_BIND b;
    memset(&b, 0, sizeof(MYSQL_BIND));
    b.buffer_type = MYSQL_TYPE_LONG;
    b.buffer = (char *)&a;  
    b.buffer_length = sizeof(a);
    b.length = &dummy1; 
    b.is_null = &dummy2;
    b.is_unsigned = true;
    if (mysql_stmt_fetch_column(stmt, &b, column_index, 0) != 0) {
      throw SQLException(SQLException::GET_FAILED, mysql_stmt_error(stmt), getQuery());
    }
  }

  return a;
}

double
MySQLStatement::getDouble(int column_index) {
  if (column_index < 0 || column_index >= MYSQL_MAX_BOUND_VARIABLES) throw SQLException(SQLException::BAD_COLUMN_INDEX, "", getQuery());
  assert(stmt);
  double a = 0;
   
  if (bind_is_null[column_index]) {
    
  } else if (bind_length[column_index]) {
    long unsigned int dummy1;
    my_bool dummy2;
    MYSQL_BIND b;
    memset(&b, 0, sizeof(MYSQL_BIND));
    b.buffer_type = MYSQL_TYPE_DOUBLE;
    b.buffer = (char *)&a;  
    b.buffer_length = sizeof(a);
    b.length = &dummy1; 
    b.is_null = &dummy2;
    if (mysql_stmt_fetch_column(stmt, &b, column_index, 0) != 0) {
      throw SQLException(SQLException::GET_FAILED, mysql_stmt_error(stmt), getQuery());
    }
  }

  return a;
}

long long
MySQLStatement::getLongLong(int column_index) {
  if (column_index < 0 || column_index >= MYSQL_MAX_BOUND_VARIABLES) throw SQLException(SQLException::BAD_COLUMN_INDEX, "", getQuery());

  assert(stmt);
  
  long long a = 0;
   
  if (bind_is_null[column_index]) {

  } else if (bind_length[column_index]) {
    long unsigned int dummy1;
    my_bool dummy2;
    MYSQL_BIND b;
    memset(&b, 0, sizeof(MYSQL_BIND));
    b.buffer_type = MYSQL_TYPE_LONGLONG;
    b.buffer = (char *)&a;  
    b.buffer_length = sizeof(a);
    b.length = &dummy1;
    b.is_null = &dummy2;
    if (mysql_stmt_fetch_column(stmt, &b, column_index, 0) != 0) {
      throw SQLException(SQLException::GET_FAILED, mysql_stmt_error(stmt), getQuery());
    }
  }

  return a;
}

string
MySQLStatement::getText(int column_index) {
  if (column_index < 0 || column_index >= MYSQL_MAX_BOUND_VARIABLES) throw SQLException(SQLException::BAD_COLUMN_INDEX, "", getQuery());

  assert(stmt);
  
  unsigned long int len = (int)bind_length[column_index];

  string s;
  
  if (bind_is_null[column_index]) {

  } else if (len) {
    char * tmp = new char[len];
    
    long unsigned int dummy1;
    my_bool dummy2;
    MYSQL_BIND b;
    memset(&b, 0, sizeof(MYSQL_BIND));
    b.buffer_type = MYSQL_TYPE_STRING;
    b.buffer = tmp;  
    b.buffer_length = len;
    b.length = &dummy1; 
    b.is_null = &dummy2;
    
    if (mysql_stmt_fetch_column(stmt, &b, column_index, 0) != 0) {
      delete[] tmp;
      throw SQLException(SQLException::GET_FAILED, mysql_stmt_error(stmt), getQuery());
    }
	
    s = string(tmp, len);
    
    delete[] tmp;
  }
  
  return s;
}

ustring
MySQLStatement::getBlob(int column_index) {
  if (column_index < 0 || column_index >= MYSQL_MAX_BOUND_VARIABLES) throw SQLException(SQLException::BAD_COLUMN_INDEX, "", getQuery());

  assert(stmt);
  
  unsigned long int len = (int)bind_length[column_index];

  ustring s;
  
  if (bind_is_null[column_index]) {

  } else if (len) {
    char * tmp = new char[len];
    
    long unsigned int dummy1;
    my_bool dummy2;
    MYSQL_BIND b;

    memset(&b, 0, sizeof(MYSQL_BIND));
    b.buffer_type = MYSQL_TYPE_BLOB;
    b.buffer = tmp;  
    b.buffer_length = len;
    b.length = &dummy1; 
    b.is_null = &dummy2;
    
    if (mysql_stmt_fetch_column(stmt, &b, column_index, 0) != 0) {
      delete[] tmp;
      throw SQLException(SQLException::GET_FAILED, mysql_stmt_error(stmt), getQuery());
    }
    s = ustring((unsigned char *)tmp, len);
    
    delete[] tmp;
  }

  return s;
}

void
MySQLStatement::bindData(int index, enum_field_types buffer_type, const void * ptr, unsigned int size, bool is_defined, bool is_unsigned) {
  index--;
  if (index < 0 || index >= num_bound_variables) {
    throw SQLException(SQLException::BAD_BIND_INDEX, "", getQuery());
    return;
  }
  char * buffer;
  if (size <= MYSQL_BIND_BUFFER_SIZE) {
    buffer = &bind_buffer[index * MYSQL_BIND_BUFFER_SIZE];
  } else {
    delete[] bind_ptr[index];
    buffer = bind_ptr[index] = new char[size];
  }
  memcpy(buffer, ptr, size);
  bind_data[index].buffer_type = buffer_type;
  bind_data[index].buffer = buffer;
  bind_data[index].buffer_length = size;
  bind_data[index].is_unsigned = is_unsigned;
  if (is_defined) {
    bind_data[index].is_null = &is_not_null;
  } else {
    bind_data[index].is_null = &is_null;
  }
}