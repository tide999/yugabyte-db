// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <ctime>
#include <iostream>
#include <sstream>

#include "yb/client/callbacks.h"
#include "yb/client/client.h"
#include "yb/client/row_result.h"
#include "yb/client/stubs.h"
#include "yb/client/value.h"
#include "yb/common/partial_row.h"

using yb::client::KuduClient;
using yb::client::KuduClientBuilder;
using yb::client::KuduColumnSchema;
using yb::client::KuduError;
using yb::client::KuduInsert;
using yb::client::KuduPredicate;
using yb::client::KuduRowResult;
using yb::client::KuduScanner;
using yb::client::KuduSchema;
using yb::client::KuduSchemaBuilder;
using yb::client::KuduSession;
using yb::client::KuduStatusFunctionCallback;
using yb::client::KuduTable;
using yb::client::KuduTableAlterer;
using yb::client::KuduTableCreator;
using yb::client::KuduValue;
using yb::client::sp::shared_ptr;
using yb::KuduPartialRow;
using yb::MonoDelta;
using yb::Status;

using std::string;
using std::stringstream;
using std::vector;

static Status CreateClient(const string& addr,
                           shared_ptr<KuduClient>* client) {
  return KuduClientBuilder()
      .add_master_server_addr(addr)
      .default_admin_operation_timeout(MonoDelta::FromSeconds(20))
      .Build(client);
}

static KuduSchema CreateSchema() {
  KuduSchema schema;
  KuduSchemaBuilder b;
  b.AddColumn("key")->Type(KuduColumnSchema::INT32)->NotNull()->PrimaryKey();
  b.AddColumn("int_val")->Type(KuduColumnSchema::INT32)->NotNull();
  b.AddColumn("string_val")->Type(KuduColumnSchema::STRING)->NotNull();
  b.AddColumn("non_null_with_default")->Type(KuduColumnSchema::INT32)->NotNull()
    ->Default(KuduValue::FromInt(12345));
  YB_CHECK_OK(b.Build(&schema));
  return schema;
}

static Status DoesTableExist(const shared_ptr<KuduClient>& client,
                             const string& table_name,
                             bool *exists) {
  shared_ptr<KuduTable> table;
  Status s = client->OpenTable(table_name, &table);
  if (s.ok()) {
    *exists = true;
  } else if (s.IsNotFound()) {
    *exists = false;
    s = Status::OK();
  }
  return s;
}

static Status CreateTable(const shared_ptr<KuduClient>& client,
                          const string& table_name,
                          const KuduSchema& schema,
                          int num_tablets) {
  // Generate the split keys for the table.
  vector<const KuduPartialRow*> splits;
  int32_t increment = 1000 / num_tablets;
  for (int32_t i = 1; i < num_tablets; i++) {
    KuduPartialRow* row = schema.NewRow();
    YB_CHECK_OK(row->SetInt32(0, i * increment));
    splits.push_back(row);
  }

  // Create the table.
  KuduTableCreator* table_creator = client->NewTableCreator();
  Status s = table_creator->table_name(table_name)
      .schema(&schema)
      .split_rows(splits)
      .Create();
  delete table_creator;
  return s;
}

static Status AlterTable(const shared_ptr<KuduClient>& client,
                         const string& table_name) {
  KuduTableAlterer* table_alterer = client->NewTableAlterer(table_name);
  table_alterer->AlterColumn("int_val")->RenameTo("integer_val");
  table_alterer->AddColumn("another_val")->Type(KuduColumnSchema::BOOL);
  table_alterer->DropColumn("string_val");
  Status s = table_alterer->Alter();
  delete table_alterer;
  return s;
}

static void StatusCB(void* unused, const Status& status) {
  YB_LOG(INFO) << "Asynchronous flush finished with status: "
                      << status.ToString();
}

static Status InsertRows(const shared_ptr<KuduTable>& table, int num_rows) {
  shared_ptr<KuduSession> session = table->client()->NewSession();
  YB_RETURN_NOT_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));
  session->SetTimeoutMillis(5000);

  for (int i = 0; i < num_rows; i++) {
    KuduInsert* insert = table->NewInsert();
    KuduPartialRow* row = insert->mutable_row();
    YB_CHECK_OK(row->SetInt32("key", i));
    YB_CHECK_OK(row->SetInt32("integer_val", i * 2));
    YB_CHECK_OK(row->SetInt32("non_null_with_default", i * 5));
    YB_CHECK_OK(session->Apply(insert));
  }
  Status s = session->Flush();
  if (s.ok()) {
    return s;
  }

  // Test asynchronous flush.
  KuduStatusFunctionCallback<void*> status_cb(&StatusCB, NULL);
  session->FlushAsync(&status_cb);

  // Look at the session's errors.
  vector<KuduError*> errors;
  bool overflow;
  session->GetPendingErrors(&errors, &overflow);
  s = overflow ? Status::IOError("Overflowed pending errors in session") :
      errors.front()->status();
  while (!errors.empty()) {
    delete errors.back();
    errors.pop_back();
  }
  YB_RETURN_NOT_OK(s);

  // Close the session.
  return session->Close();
}

static Status ScanRows(const shared_ptr<KuduTable>& table) {
  const int kLowerBound = 5;
  const int kUpperBound = 600;

  KuduScanner scanner(table.get());

  // Add a predicate: WHERE key >= 5
  KuduPredicate* p = table->NewComparisonPredicate(
      "key", KuduPredicate::GREATER_EQUAL, KuduValue::FromInt(kLowerBound));
  YB_RETURN_NOT_OK(scanner.AddConjunctPredicate(p));

  // Add a predicate: WHERE key <= 600
  p = table->NewComparisonPredicate(
      "key", KuduPredicate::LESS_EQUAL, KuduValue::FromInt(kUpperBound));
  YB_RETURN_NOT_OK(scanner.AddConjunctPredicate(p));

  YB_RETURN_NOT_OK(scanner.Open());
  vector<KuduRowResult> results;

  int next_row = kLowerBound;
  while (scanner.HasMoreRows()) {
    YB_RETURN_NOT_OK(scanner.NextBatch(&results));
    for (vector<KuduRowResult>::iterator iter = results.begin();
        iter != results.end();
        iter++, next_row++) {
      const KuduRowResult& result = *iter;
      int32_t val;
      YB_RETURN_NOT_OK(result.GetInt32("key", &val));
      if (val != next_row) {
        stringstream out;
        out << "Scan returned the wrong results. Expected key "
            << next_row << " but got " << val;
        return Status::IOError(out.str());
      }
    }
    results.clear();
  }

  // next_row is now one past the last row we read.
  int last_row_seen = next_row - 1;

  if (last_row_seen != kUpperBound) {
    stringstream out;
    out << "Scan returned the wrong results. Expected last row to be "
        << kUpperBound << " rows but got " << last_row_seen;
    return Status::IOError(out.str());
  }
  return Status::OK();
}

static void LogCb(void* unused,
                  yb::client::KuduLogSeverity severity,
                  const char* filename,
                  int line_number,
                  const struct ::tm* time,
                  const char* message,
                  size_t message_len) {
  YB_LOG(INFO) << "Received log message from Kudu client library";
  YB_LOG(INFO) << " Severity: " << severity;
  YB_LOG(INFO) << " Filename: " << filename;
  YB_LOG(INFO) << " Line number: " << line_number;
  char time_buf[32];
  // Example: Tue Mar 24 11:46:43 2015.
  YB_CHECK(strftime(time_buf, sizeof(time_buf), "%a %b %d %T %Y", time));
  YB_LOG(INFO) << " Time: " << time_buf;
  YB_LOG(INFO) << " Message: " << string(message, message_len);
}

int main(int argc, char* argv[]) {
  yb::client::KuduLoggingFunctionCallback<void*> log_cb(&LogCb, NULL);
  yb::client::InstallLoggingCallback(&log_cb);

  if (argc != 2) {
    YB_LOG(FATAL) << "usage: " << argv[0] << " <master host>";
  }
  const string master_host = argv[1];

  const string kTableName = "test_table";

  // Enable verbose debugging for the client library.
  yb::client::SetVerboseLogLevel(2);

  // Create and connect a client.
  shared_ptr<KuduClient> client;
  YB_CHECK_OK(CreateClient(master_host, &client));
  YB_LOG(INFO) << "Created a client connection";

  // Disable the verbose logging.
  yb::client::SetVerboseLogLevel(0);

  // Create a schema.
  KuduSchema schema(CreateSchema());
  YB_LOG(INFO) << "Created a schema";

  // Create a table with that schema.
  bool exists;
  YB_CHECK_OK(DoesTableExist(client, kTableName, &exists));
  if (exists) {
    client->DeleteTable(kTableName);
    YB_LOG(INFO) << "Deleting old table before creating new one";
  }
  YB_CHECK_OK(CreateTable(client, kTableName, schema, 10));
  YB_LOG(INFO) << "Created a table";

  // Alter the table.
  YB_CHECK_OK(AlterTable(client, kTableName));
  YB_LOG(INFO) << "Altered a table";

  // Insert some rows into the table.
  shared_ptr<KuduTable> table;
  YB_CHECK_OK(client->OpenTable(kTableName, &table));
  YB_CHECK_OK(InsertRows(table, 1000));
  YB_LOG(INFO) << "Inserted some rows into a table";

  // Scan some rows.
  YB_CHECK_OK(ScanRows(table));
  YB_LOG(INFO) << "Scanned some rows out of a table";

  // Delete the table.
  YB_CHECK_OK(client->DeleteTable(kTableName));
  YB_LOG(INFO) << "Deleted a table";

  // Done!
  YB_LOG(INFO) << "Done";
  return 0;
}
