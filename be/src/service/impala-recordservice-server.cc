// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "service/impala-server.h"
#include "service/impala-server.inline.h"

#include <algorithm>
#include <boost/algorithm/string/join.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/unordered_set.hpp>
#include <thrift/protocol/TDebugProtocol.h>
#include <boost/foreach.hpp>
#include <boost/algorithm/string.hpp>
#include <gutil/strings/substitute.h>

#include "common/logging.h"
#include "common/version.h"
#include "exprs/expr.h"
#include "exprs/expr-context.h"
#include "exprs/slot-ref.h"
#include "runtime/hdfs-fs-cache.h"
#include "runtime/raw-value.h"
#include "service/query-exec-state.h"
#include "service/query-options.h"
#include "rpc/thrift-util.h"
#include "util/debug-util.h"
#include "util/impalad-metrics.h"
#include "util/hdfs-util.h"
#include "service/hs2-util.h"

using namespace std;
using namespace boost;
using namespace strings;
using namespace beeswax; // Converting QueryState

DECLARE_int32(recordservice_worker_port);

// This value has a big impact on performance. For simple queries (1 bigint col),
// 5000 is a 2x improvement over a fetch size of 1024.
// TODO: investigate more
static const int DEFAULT_FETCH_SIZE = 5000;

// Names of temporary tables used to service path based requests.
// TODO: everything about temp tables is a hack.
static const char* TEMP_DB = "rs_tmp_db";
static const char* TEMP_TBL = "tmp_tbl";

namespace impala {

inline void ThrowException(const recordservice::TErrorCode::type& code,
    const string& msg) {
  recordservice::TRecordServiceException ex;
  ex.code = code;
  ex.message = msg;
  throw ex;
}

inline void ThrowFetchException(const Status& status) {
  DCHECK(!status.ok());
  recordservice::TRecordServiceException ex;
  if (status.IsCancelled()) {
    ex.code = recordservice::TErrorCode::CANCELLED;
    ex.message = "Task failed because it was cancelled.";
  } else if (status.IsMemLimitExceeded()) {
    ex.code = recordservice::TErrorCode::OUT_OF_MEMORY;
    ex.message = "Task failed because it ran out of memory.";
  } else {
    ex.code = recordservice::TErrorCode::INTERNAL_ERROR;
    ex.message = "Task failed due to an internal error.";
  }
  ex.__set_detail(status.GetErrorMsg());
}

// Base class for test result set serializations. The functions in here and
// not used in the record service path.
//
// Used to abstract away serializing results. The calling pattern is:
//
// BaseResult* result = new ...
// result->Init();
// for each rpc:
//   result->SetReturnBuffer();
//   for each batch:
//     result->AddBatch()
//   result->FinalizeResult()
class ImpalaServer::BaseResultSet : public ImpalaServer::QueryResultSet {
 public:
  // Convert TResultRow to ASCII using "\t" as column delimiter and store it in this
  // result set.
  virtual Status AddOneRow(const TResultRow& row) {
    CHECK(false) << "Not used";
    return Status::OK;
  }

  virtual int AddRows(const QueryResultSet* other, int start_idx, int num_rows) {
    CHECK(false) << "Not used";
    return num_rows;
  }

  virtual int64_t ByteSize(int start_idx, int num_rows) {
    CHECK(false) << "Not used";
    return sizeof(int64_t);
  }

  virtual Status AddOneRow(const vector<void*>& col_values, const vector<int>& scales) {
    CHECK(false) << "Not used";
    return Status::OK;
  }

  virtual void Init(const TResultSetMetadata& md, int fetch_size) {
    for (int i = 0; i < md.columns.size(); ++i) {
      types_.push_back(md.columns[i].columnType);
      if (types_[i] == TYPE_TIMESTAMP) {
        type_sizes_.push_back(12);
      } else {
        type_sizes_.push_back(types_[i].GetByteSize());
      }
    }
  }

  virtual void FinalizeResult() {}

  virtual bool supports_batch_add() const { return true; }

  // This should be set for every fetch request so that the results are directly
  // populated in the thrift result object (to avoid a copy).
  virtual void SetReturnBuffer(recordservice::TFetchResult* result) = 0;

  virtual size_t size() { return result_ == NULL ? 0 :result_->num_rows; }

 protected:
  BaseResultSet() : result_(NULL) {}
  recordservice::TFetchResult* result_;

  vector<ColumnType> types_;
  vector<int> type_sizes_;
};

// Additional state for the record service. Put here instead of QueryExecState
// to separate from Impala code.
class ImpalaServer::RecordServiceTaskState {
 public:
  RecordServiceTaskState() : counters_initialized(false) {}

  int fetch_size;

  recordservice::TRowBatchFormat::type format;
  scoped_ptr<ImpalaServer::BaseResultSet> results;

  // Populated on first call to Fetch(). At that point the query has for sure
  // made enough progress that the counters are initialized.
  bool counters_initialized;
  RuntimeProfile::Counter* serialize_timer;
  RuntimeProfile::Counter* client_timer;

  RuntimeProfile::Counter* bytes_assigned_counter;
  RuntimeProfile::Counter* bytes_read_counter;
  RuntimeProfile::Counter* bytes_read_local_counter;
  RuntimeProfile::Counter* rows_read_counter;
  RuntimeProfile::Counter* rows_returned_counter;
  RuntimeProfile::Counter* decompression_timer;
  RuntimeProfile::Counter* hdfs_throughput_counter;
};

// This is the parquet plain encoding, meaning we append the little endian version
// of the value to the end of the buffer.
class ImpalaServer::RecordServiceParquetResultSet : public ImpalaServer::BaseResultSet {
 public:
  RecordServiceParquetResultSet(bool all_slot_refs,
      const vector<ExprContext*>& output_exprs) : all_slot_refs_(all_slot_refs) {
    if (all_slot_refs) {
      slot_descs_.resize(output_exprs.size());
      for (int i = 0; i < output_exprs.size(); ++i) {
        SlotRef* slot_ref = reinterpret_cast<SlotRef*>(output_exprs[i]->root());
        slot_descs_[i].byte_offset = slot_ref->slot_offset();
        slot_descs_[i].null_offset = slot_ref->null_indicator();
      }
    }
  }

  virtual void SetReturnBuffer(recordservice::TFetchResult* result) {
    result_ = result;
    result_->__isset.columnar_row_batch = true;
    result_->columnar_row_batch.cols.resize(types_.size());
  }

  virtual void AddRowBatch(RowBatch* input, int row_idx, int num_rows,
      vector<ExprContext*>* ctxs) {
    DCHECK(result_->__isset.columnar_row_batch);
    recordservice::TColumnarRowBatch& batch = result_->columnar_row_batch;

    if (all_slot_refs_) {
      // In this case, all the output exprs are slot refs and we want to serialize them
      // to the record service format. To do this we:
      // 1. Reserve the outgoing buffer to the max size (for fixed length types).
      // 2. Append the current value to the outgoing buffer.
      // 3. Resize the outgoing buffer when we are done with the row batch (which
      // can be sparse due to NULLs).
      DCHECK_EQ(ctxs->size(), slot_descs_.size());
      const int num_cols = slot_descs_.size();

      // Reserve the size of the output where possible.
      for (int c = 0; c < num_cols; ++c) {
        DCHECK_EQ(batch.cols[c].is_null.size(), 0);
        DCHECK_EQ(batch.cols[c].data.size(), 0);

        batch.cols[c].is_null.resize(num_rows);
        if (type_sizes_[c] != 0) {
          batch.cols[c].data.resize(num_rows * type_sizes_[c]);
        }
      }

      // Only used for fixed length types. data[c] is the ptr that the next value
      // should be appended at.
      char* data[num_cols];
      for (int c = 0; c < num_cols; ++c) {
        data[c] = (char*)batch.cols[c].data.data();
      }

      // This loop is extremely perf sensitive.
      for (int i = 0; i < num_rows; ++i) {
        Tuple* tuple = input->GetRow(row_idx++)->GetTuple(0);
        for (int c = 0; c < num_cols; ++c) {
          bool is_null = tuple->IsNull(slot_descs_[c].null_offset);
          batch.cols[c].is_null[i] = is_null;
          if (is_null) continue;

          const int type_size = type_sizes_[c];
          if (type_size == 0) {
            // TODO: this resizing can't be good. The rowbatch should keep track of
            // how long the string data is.
            string& dst = batch.cols[c].data;
            int offset = dst.size();
            const StringValue* sv = tuple->GetStringSlot(slot_descs_[c].byte_offset);
            int len = sv->len + sizeof(int32_t);
            dst.resize(offset + len);
            memcpy((char*)dst.data() + offset, &sv->len, sizeof(int32_t));
            memcpy((char*)dst.data() + offset + sizeof(int32_t), sv->ptr, sv->len);
          } else {
            const void* slot = tuple->GetSlot(slot_descs_[c].byte_offset);
            if (types_[c] == TYPE_TIMESTAMP) {
              DCHECK_EQ(type_size, 12);
              const TimestampValue* ts = reinterpret_cast<const TimestampValue*>(slot);
              int64_t millis;
              int32_t nanos;
              ts->ToMillisAndNanos(&millis, &nanos);
              memcpy(data[c], &millis, sizeof(int64_t));
              memcpy(data[c] + sizeof(int64_t), &nanos, sizeof(int32_t));
            } else {
              memcpy(data[c], slot, type_size);
            }
            data[c] += type_size;
          }
        }
      }

      // For fixed-length columns, shrink the size if necessary. In the case of NULLs,
      // we could have resized the buffer bigger than necessary.
      for (int c = 0; c < num_cols; ++c) {
        if (type_sizes_[c] == 0) continue;
        int size = data[c] - batch.cols[c].data.data();
        if (batch.cols[c].data.size() != size) {
          batch.cols[c].data.resize(size);
        }
      }
    } else {
      // Reserve the size of the output where possible.
      for (int c = 0; c < ctxs->size(); ++c) {
        DCHECK_EQ(batch.cols[c].is_null.size(), 0);
        DCHECK_EQ(batch.cols[c].data.size(), 0);

        batch.cols[c].is_null.reserve(num_rows);
        if (type_sizes_[c] != 0) {
          batch.cols[c].data.reserve(num_rows * type_sizes_[c]);
        }
      }
      for (int i = 0; i < num_rows; ++i) {
        TupleRow* row = input->GetRow(row_idx++);

        for (int c = 0; c < ctxs->size(); ++c) {
          const void* v = (*ctxs)[c]->GetValue(row);
          batch.cols[c].is_null.push_back(v == NULL);
          if (v == NULL) continue;

          string& data = batch.cols[c].data;
          int offset = data.size();

          // Encode the values here. For non-string types, just write the value as
          // little endian. For strings, it is the length(little endian) followed
          // by the string.
          const int type_size = type_sizes_[c];
          if (type_size == 0) {
            const StringValue* sv = reinterpret_cast<const StringValue*>(v);
            int len = sv->len + sizeof(int32_t);
            data.resize(offset + len);
            memcpy((char*)data.data() + offset, &sv->len, sizeof(int32_t));
            memcpy((char*)data.data() + offset + sizeof(int32_t), sv->ptr, sv->len);
          } else {
            data.resize(offset + type_size);
            memcpy((char*)data.data() + offset, v, type_size);
          }
        }
      }
    }
    result_->num_rows += num_rows;
  }

 private:
  struct SlotDesc {
    // Byte offset in tuple
    int byte_offset;
    NullIndicatorOffset null_offset;

    SlotDesc() : null_offset(0, 0) {}
  };

  // If true, all the output exprs are slot refs.
  bool all_slot_refs_;

  // Cache of the slot desc. Only set if all_slot_refs_ is true. We'll use this
  // intead of the exprs (for performance).
  vector<SlotDesc> slot_descs_;
};

shared_ptr<ImpalaServer::SessionState> ImpalaServer::GetRecordServiceSession() {
  unique_lock<mutex> l(connection_to_sessions_map_lock_);
  if (record_service_session_.get() == NULL) {
    record_service_session_.reset(new SessionState());
    record_service_session_->session_type = TSessionType::RECORDSERVICE;
    record_service_session_->start_time = TimestampValue::local_time();
    record_service_session_->last_accessed_ms = ms_since_epoch();
    record_service_session_->database = "default";
    record_service_session_->ref_count = 1;
  }
  return record_service_session_;
}

recordservice::TType ToRecordServiceType(const ColumnType& t) {
  recordservice::TType result;
  switch (t.type) {
    case TYPE_BOOLEAN:
      result.type_id = recordservice::TTypeId::BOOLEAN;
      break;
    case TYPE_TINYINT:
      result.type_id = recordservice::TTypeId::TINYINT;
      break;
    case TYPE_SMALLINT:
      result.type_id = recordservice::TTypeId::SMALLINT;
      break;
    case TYPE_INT:
      result.type_id = recordservice::TTypeId::INT;
      break;
    case TYPE_BIGINT:
      result.type_id = recordservice::TTypeId::BIGINT;
      break;
    case TYPE_FLOAT:
      result.type_id = recordservice::TTypeId::FLOAT;
      break;
    case TYPE_DOUBLE:
      result.type_id = recordservice::TTypeId::DOUBLE;
      break;
    case TYPE_STRING:
      result.type_id = recordservice::TTypeId::STRING;
      break;
    case TYPE_VARCHAR:
      result.type_id = recordservice::TTypeId::VARCHAR;
      result.__set_len(t.len);
      break;
    case TYPE_CHAR:
      result.type_id = recordservice::TTypeId::CHAR;
      result.__set_len(t.len);
      break;
    case TYPE_TIMESTAMP:
      result.type_id = recordservice::TTypeId::TIMESTAMP_NANOS;
      break;
    case TYPE_DECIMAL:
      result.type_id = recordservice::TTypeId::DECIMAL;
      result.__set_precision(t.precision);
      result.__set_scale(t.scale);
      break;
    default:
      ThrowException(recordservice::TErrorCode::INVALID_REQUEST,
          "Not supported type.");
  }
    return result;
}

static void PopulateResultSchema(const TResultSetMetadata& metadata,
    recordservice::TSchema* schema) {
  schema->cols.resize(metadata.columns.size());
  for (int i = 0; i < metadata.columns.size(); ++i) {
    ColumnType type(metadata.columns[i].columnType);
    schema->cols[i].type = ToRecordServiceType(type);
    schema->cols[i].name = metadata.columns[i].columnName;
  }
}

recordservice::TProtocolVersion::type ImpalaServer::GetProtocolVersion() {
  return recordservice::TProtocolVersion::V1;
}

//
// RecordServicePlanner
//
void ImpalaServer::PlanRequest(recordservice::TPlanRequestResult& return_val,
  const recordservice::TPlanRequestParams& req) {
  if (IsOffline()) {
    ThrowException(recordservice::TErrorCode::SERVICE_BUSY,
        "This RecordServicePlanner is not ready to accept requests."
        " Retry your request later.");
  }

  TQueryCtx query_ctx;
  // Setting num_nodes = 1 means we generate a single node plan which has
  // a simpler structure. It also prevents Impala from analyzing multi-table
  // queries, i.e. joins.
  query_ctx.request.query_options.__set_num_nodes(1);

  // Disable codegen. Codegen works well for Impala because each fragment processes
  // multiple blocks, so the cost of codegen is amortized.
  // TODO: implement codegen caching.
  query_ctx.request.query_options.__set_disable_codegen(true);

  unique_lock<mutex> tmp_tbl_lock;

  switch (req.request_type) {
    case recordservice::TRequestType::Sql:
      query_ctx.request.stmt = req.sql_stmt;
      break;
    case recordservice::TRequestType::Path: {
      // TODO: improve tmp table management or get impala to do it properly.
      unique_lock<mutex> l(tmp_tbl_lock_);
      tmp_tbl_lock.swap(l);

      string tmp_table;
      Status status = CreateTmpTable(req.path, &tmp_table);
      if (!status.ok()) {
        ThrowException(recordservice::TErrorCode::INVALID_REQUEST, status.GetErrorMsg());
      }
      if (req.path.__isset.query) {
        string query = req.path.query;
        size_t p = req.path.query.find("__PATH__");
        if (p == string::npos) {
          ThrowException(recordservice::TErrorCode::INVALID_REQUEST,
              "Query request must contain __PATH__: " + query);
        }
        query.replace(p, 8, tmp_table);
        query_ctx.request.stmt = query;
      } else {
        query_ctx.request.stmt = "SELECT * FROM " + tmp_table;
      }
      break;
    }
    default:
      ThrowException(recordservice::TErrorCode::INVALID_REQUEST,
          "Unsupported request types. Supported request types are: SQL");
  }

  LOG(ERROR) << "RecordService::PlanRequest: " << query_ctx.request.stmt;

  // Plan the request.
  TRecordServiceExecRequest result;
  Status status = exec_env_->frontend()->GetRecordServiceExecRequest(query_ctx, &result);
  if (tmp_tbl_lock.owns_lock()) tmp_tbl_lock.unlock();

  if (!status.ok()) {
    ThrowException(recordservice::TErrorCode::INVALID_REQUEST,
        status.GetErrorMsg());
  }

  if (result.stmt_type != TStmtType::QUERY) {
    ThrowException(recordservice::TErrorCode::INVALID_REQUEST,
        "Cannot run non-SELECT statements");
  }

  // Extract the types of the result.
  DCHECK(result.__isset.result_set_metadata);
  PopulateResultSchema(result.result_set_metadata, &return_val.schema);

  // Walk the plan to compute the tasks. We want to find the scan nodes
  // and distribute them.
  const vector<TQueryExecRequest>& query_requests = result.query_exec_request;

  // The TRecordServiceExecRequest will contain a bunch of TQueryRequest
  // objects.. each corresponding to a PlanFragment. This is then reconstituted
  // into a list of TExecRequests (again one for each PlanFragment). Each
  // TExecRequest is then serialized and set as the "task" field of the
  // TTask object.
  // TODO : we would need to encrypt the TExecRequest object for security
  for (int i = 0; i < query_requests.size(); ++i) {
    recordservice::TTask task;
    // Fill with newly constructed TExecPlan
    int buffer_size = 100 * 1024;  // start out with 100KB
    ThriftSerializer serializer(false, buffer_size);

    TExecRequest req;
    req.__set_stmt_type(result.stmt_type);
    req.__set_access_events(result.access_events);
    // TODO : Analysis warnings and explain results should not be
    // sent back to the worker.. move this out in such a way that
    // only the planner client gets this.
    req.__set_analysis_warnings(result.analysis_warnings);
    req.__set_explain_result(result.explain_result);
    req.__set_query_exec_request(query_requests[i]);
    req.__set_query_options(result.query_options);
    req.__set_result_set_metadata(result.result_set_metadata);
    req.__set_set_query_option_request(result.set_query_option_request);

    serializer.Serialize<TExecRequest>(&req, &task.task);

    pair<TPlanNodeId, vector<TScanRangeLocations> > entry;
    TScanRangeLocations locs;
    TScanRangeLocation loc;
    BOOST_FOREACH(entry, query_requests[i].per_node_scan_ranges) {
      // Assuming for most cases, only 1 plan node (scan node)
      // will be present per fragment..
      // If the assumption is not true, it will not generally be
      // too much of a problem, only that some reads will become
      // non-local
      BOOST_FOREACH(locs, entry.second) {
        BOOST_FOREACH(loc, locs.locations) {
          recordservice::TNetworkAddress host;
          host.hostname = query_requests[i].host_list[loc.host_idx].hostname;
          // TODO: this port should come from the membership information.
          host.port = FLAGS_recordservice_worker_port;
          task.local_hosts.push_back(host);
        }
      }
    }
    return_val.tasks.push_back(task);
  }

  // TODO: this port should come from the membership information and return all hosts
  // the workers are running on.
  recordservice::TNetworkAddress default_host;
  default_host.hostname = "localhost";
  default_host.port = FLAGS_recordservice_worker_port;
  return_val.hosts.push_back(default_host);
}

//
// RecordServiceWorker
//
void ImpalaServer::ExecTask(recordservice::TExecTaskResult& return_val,
    const recordservice::TExecTaskParams& req) {
  if (IsOffline()) {
    ThrowException(recordservice::TErrorCode::SERVICE_BUSY,
        "This RecordServicePlanner is not ready to accept request."
        " Retry your request later.");
  }

  GetRecordServiceSession();

  shared_ptr<RecordServiceTaskState> task_state(new RecordServiceTaskState());
  TExecRequest exec_req;
  uint32_t size;
  size = req.task.size();
  Status status = DeserializeThriftMsg(reinterpret_cast<const uint8_t*>(&req.task[0]),
      &size, false, &exec_req);
  if (!status.ok()) {
    ThrowException(recordservice::TErrorCode::INVALID_REQUEST, status.GetErrorMsg());
  }
  LOG(INFO) << "RecordService::ExecRequest: query plan " <<
          exec_req.query_exec_request.query_plan;

  task_state->fetch_size = DEFAULT_FETCH_SIZE;
  if (req.__isset.fetch_size) task_state->fetch_size = req.fetch_size;
  exec_req.query_exec_request.
      query_ctx.request.query_options.__set_batch_size(task_state->fetch_size);

  shared_ptr<QueryExecState> exec_state;
  status = ExecuteRecordServiceRequest(
          &exec_req.query_exec_request.query_ctx,
          &exec_req, record_service_session_, &exec_state);
  if (!status.ok()) {
    ThrowException(recordservice::TErrorCode::INVALID_REQUEST, status.GetErrorMsg());
  }

  PopulateResultSchema(*exec_state->result_metadata(), &return_val.schema);
  exec_state->SetRecordServiceTaskState(task_state);

  // Optimization if the result exprs are all just "simple" slot refs. This means
  // that they contain a single non-nullable tuple row. This is the common case and
  // we can simplify the row serialization logic.
  // TODO: this should be replaced by codegen to handle all the cases.
  bool all_slot_refs = true;
  const vector<ExprContext*>& output_exprs = exec_state->output_exprs();
  for (int i = 0; i < output_exprs.size(); ++i) {
    if (output_exprs[i]->root()->is_slotref()) {
      SlotRef* slot_ref = reinterpret_cast<SlotRef*>(output_exprs[i]->root());
      if (!slot_ref->tuple_is_nullable() && slot_ref->tuple_idx() == 0) continue;
    }
    all_slot_refs = false;
    break;
  }

  task_state->format = recordservice::TRowBatchFormat::Columnar;
  if (req.__isset.row_batch_format) task_state->format = req.row_batch_format;
  switch (task_state->format) {
    case recordservice::TRowBatchFormat::Columnar:
      task_state->results.reset(new RecordServiceParquetResultSet(
          all_slot_refs, output_exprs));
      break;
    default:
      ThrowException(recordservice::TErrorCode::INVALID_REQUEST,
          "Service does not support this row batch format.");
  }
  task_state->results->Init(*exec_state->result_metadata(), task_state->fetch_size);

  exec_state->UpdateQueryState(QueryState::RUNNING);
  exec_state->WaitAsync();
  status = SetQueryInflight(record_service_session_, exec_state);
  if (!status.ok()) {
    UnregisterQuery(exec_state->query_id(), false, &status);
  }
  return_val.handle.hi = exec_state->query_id().hi;
  return_val.handle.lo = exec_state->query_id().lo;
}

// Computes the percent of num/denom
static double ComputeCompletionPercentage(RuntimeProfile::Counter* num,
    RuntimeProfile::Counter* denom) {
  if (num == NULL || denom == NULL || denom->value() == 0) return 0;
  double result = (double)num->value() / (double)denom->value();
  if (result > 1) result = 1;
  return result * 100;
}

void ImpalaServer::Fetch(recordservice::TFetchResult& return_val,
    const recordservice::TFetchParams& req) {
  TUniqueId query_id;
  query_id.hi = req.handle.hi;
  query_id.lo = req.handle.lo;

  shared_ptr<QueryExecState> exec_state = GetQueryExecState(query_id, false);
  if (exec_state.get() == NULL) {
    ThrowException(recordservice::TErrorCode::INVALID_HANDLE, "Invalid handle");
  }

  RecordServiceTaskState* task_state = exec_state->record_service_task_state();
  exec_state->BlockOnWait();

  task_state->results->SetReturnBuffer(&return_val);

  lock_guard<mutex> frl(*exec_state->fetch_rows_lock());
  lock_guard<mutex> l(*exec_state->lock());

  Status status = exec_state->FetchRows(
      task_state->fetch_size, task_state->results.get());
  if (!status.ok()) ThrowFetchException(status);

  if (!task_state->counters_initialized) {
    // First time the client called fetch. Extract the counters.
    RuntimeProfile* server_profile = exec_state->server_profile();

    task_state->serialize_timer = server_profile->GetCounter("RowMaterializationTimer");
    task_state->client_timer = server_profile->GetCounter("ClientFetchWaitTimer");

    RuntimeProfile* coord_profile = exec_state->coord()->query_profile();
    vector<RuntimeProfile*> children;
    coord_profile->GetAllChildren(&children);
    for (int i = 0; i < children.size(); ++i) {
      if (children[i]->name() != "HDFS_SCAN_NODE (id=0)") continue;
      RuntimeProfile* profile = children[i];
      task_state->bytes_assigned_counter = profile->GetCounter("BytesAssigned");
      task_state->bytes_read_counter = profile->GetCounter("BytesRead");
      task_state->bytes_read_local_counter = profile->GetCounter("BytesReadLocal");
      task_state->rows_read_counter = profile->GetCounter("RowsRead");
      task_state->rows_returned_counter = profile->GetCounter("RowsReturned");
      task_state->decompression_timer = profile->GetCounter("DecompressionTime");
      task_state->hdfs_throughput_counter =
        profile->GetCounter("PerReadThreadRawHdfsThroughput");
    }
    task_state->counters_initialized = true;
  }

  return_val.done = exec_state->eos();
  return_val.task_completion_percentage = ComputeCompletionPercentage(
      task_state->bytes_read_counter, task_state->bytes_assigned_counter);
  return_val.row_batch_format = task_state->format;

  task_state->results->FinalizeResult();
}

void ImpalaServer::CloseTask(const recordservice::TUniqueId& req) {
  TUniqueId query_id;
  query_id.hi = req.hi;
  query_id.lo = req.lo;

  shared_ptr<QueryExecState> exec_state = GetQueryExecState(query_id, false);
  if (exec_state.get() == NULL) return;
  Status status = CancelInternal(query_id, true);
  if (!status.ok()) return;
  UnregisterQuery(query_id, true);
}

// Macros to convert from runtime profile counters to metrics object.
// Also does unit conversion (i.e. STAT_MS converts to millis).
#define SET_STAT_MS_FROM_COUNTER(counter, stat_name)\
  if (counter != NULL) return_val.__set_##stat_name(counter->value() / 1000000)

#define SET_STAT_FROM_COUNTER(counter, stat_name)\
  if (counter != NULL) return_val.__set_##stat_name(counter->value())

void ImpalaServer::GetTaskStats(recordservice::TStats& return_val,
      const recordservice::TUniqueId& req) {
  TUniqueId query_id;
  query_id.hi = req.hi;
  query_id.lo = req.lo;

  // TODO: should this grab the lock in GetQueryExecState()?
  shared_ptr<QueryExecState> exec_state = GetQueryExecState(query_id, false);
  if (exec_state.get() == NULL) {
    ThrowException(recordservice::TErrorCode::INVALID_HANDLE, "Invalid handle");
  }

  lock_guard<mutex> l(*exec_state->lock());

  RecordServiceTaskState* task_state = exec_state->record_service_task_state();
  if (!task_state->counters_initialized) {
    // Task hasn't started enough to have counters.
    return;
  }

  // Populate the results from the counters.
  return_val.__set_completion_percentage(ComputeCompletionPercentage(
      task_state->bytes_read_counter, task_state->bytes_assigned_counter));
  SET_STAT_MS_FROM_COUNTER(task_state->serialize_timer, serialize_time_ms);
  SET_STAT_MS_FROM_COUNTER(task_state->client_timer, client_time_ms);
  SET_STAT_FROM_COUNTER(task_state->bytes_read_counter, bytes_read);
  SET_STAT_FROM_COUNTER(task_state->bytes_read_local_counter, bytes_read_local);
  SET_STAT_FROM_COUNTER(task_state->rows_read_counter, num_rows_read);
  SET_STAT_FROM_COUNTER(task_state->rows_returned_counter, num_rows_returned);
  SET_STAT_MS_FROM_COUNTER(task_state->decompression_timer, decompress_time_ms);
  SET_STAT_FROM_COUNTER(task_state->hdfs_throughput_counter, hdfs_throughput);
}

Status ImpalaServer::CreateTmpTable(const recordservice::TPathRequest& request,
    string* table_name) {

  hdfsFS fs;
  Status status = HdfsFsCache::instance()->GetConnection("default", &fs);
  if (!status.ok()) {
    // TODO: more error detail
    ThrowException(recordservice::TErrorCode::INTERNAL_ERROR,
        "Could not connect to HDFS");
  }

  // TODO: this should do better globbing.
  string path = request.path;
  if (path[path.size() - 1] == '*') {
    path = path.substr(0, path.size() - 1);
  }

  bool is_directory = false;
  status = IsDirectory(fs, path.c_str(), &is_directory);
  if (!status.ok()) {
    ThrowException(recordservice::TErrorCode::INVALID_REQUEST,
        "No such file or directory: " + path);
  }

  if (!is_directory) {
    // TODO: Impala should support LOCATIONs that are not directories.
    ThrowException(recordservice::TErrorCode::INVALID_REQUEST,
        "Path must be a directory: " + path);
  }

  GetRecordServiceSession();
  stringstream tbl_name;
  tbl_name << TEMP_DB << "." << TEMP_TBL;
  *table_name = tbl_name.str();

  // For now, we'll just always use one temp table.
  string commands[] = {
    "DROP TABLE IF EXISTS " + *table_name,
    "CREATE DATABASE IF NOT EXISTS " + string(TEMP_DB),
    // TODO: assume text for now. How do we best handle this?
    "CREATE EXTERNAL TABLE " + *table_name +
        "(record STRING) LOCATION \"" + path + "\"",
  };

  int num_commands = sizeof(commands) / sizeof(commands[0]);
  for (int i = 0; i < num_commands; ++i) {
    TQueryCtx query_ctx;
    query_ctx.request.stmt = commands[i];

    shared_ptr<QueryExecState> exec_state;
    RETURN_IF_ERROR(Execute(&query_ctx, record_service_session_, &exec_state));
    exec_state->UpdateQueryState(QueryState::RUNNING);

    Status status = SetQueryInflight(record_service_session_, exec_state);
    if (!status.ok()) {
      UnregisterQuery(exec_state->query_id(), false, &status);
      return status;
    }

    // block until results are ready
    exec_state->Wait();
    status = exec_state->query_status();
    if (!status.ok()) {
      UnregisterQuery(exec_state->query_id(), false, &status);
      return status;
    }
    exec_state->UpdateQueryState(QueryState::FINISHED);
    UnregisterQuery(exec_state->query_id(), true);
  }

  return Status::OK;
}

}
