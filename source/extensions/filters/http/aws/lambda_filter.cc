#include "extensions/filters/http/aws/lambda_filter.h"

#include <algorithm>
#include <list>
#include <string>
#include <vector>

#include "envoy/http/header_map.h"

#include "common/common/empty_string.h"
#include "common/common/hex.h"
#include "common/common/utility.h"
#include "common/http/filter_utility.h"
#include "common/http/headers.h"
#include "common/http/solo_filter_utility.h"
#include "common/http/utility.h"

namespace Envoy {
namespace Http {

const LowerCaseString LambdaFilter::INVOCATION_TYPE("x-amz-invocation-type");
const std::string LambdaFilter::INVOCATION_TYPE_EVENT("Event");
const std::string LambdaFilter::INVOCATION_TYPE_REQ_RESP("RequestResponse");
const LowerCaseString LambdaFilter::LOG_TYPE("x-amz-log-type");
const std::string LambdaFilter::LOG_NONE("None");

LambdaFilter::LambdaFilter(FunctionRetrieverSharedPtr retreiver)
    : function_retriever_(retreiver) {}

LambdaFilter::~LambdaFilter() {}

std::string LambdaFilter::functionUrlPath() {

  const auto &current_function = current_function_.value();
  std::stringstream val;
  val << "/2015-03-31/functions/" << (*current_function.name_)
      << "/invocations";
  if (current_function.qualifier_.has_value()) {
    val << "?Qualifier=" << (*current_function.qualifier_.value());
  }
  return val.str();
}

FilterHeadersStatus LambdaFilter::decodeHeaders(HeaderMap &headers,
                                                bool end_stream) {
  // TODO(talnordan): Provide `DETAILS`.
  RELEASE_ASSERT(current_function_.has_value(), "");

  const auto &current_function = current_function_.value();

  aws_authenticator_.init(current_function.access_key_,
                          current_function.secret_key_);
  request_headers_ = &headers;

  request_headers_->insertMethod().value().setReference(
      Headers::get().MethodValues.Post);

  //  request_headers_->removeContentLength();
  request_headers_->insertPath().value(functionUrlPath());

  if (end_stream) {
    lambdafy();
    return FilterHeadersStatus::Continue;
  }

  return FilterHeadersStatus::StopIteration;
}

FilterDataStatus LambdaFilter::decodeData(Buffer::Instance &data,
                                          bool end_stream) {
  if (!current_function_.has_value()) {
    return FilterDataStatus::Continue;
  }
  aws_authenticator_.updatePayloadHash(data);

  if (end_stream) {
    lambdafy();
    return FilterDataStatus::Continue;
  }

  return FilterDataStatus::StopIterationAndBuffer;
}

FilterTrailersStatus LambdaFilter::decodeTrailers(HeaderMap &) {
  if (current_function_.has_value()) {
    lambdafy();
  }

  return FilterTrailersStatus::Continue;
}

bool LambdaFilter::retrieveFunction(const MetadataAccessor &meta_accessor) {
  current_function_ = function_retriever_->getFunction(meta_accessor);
  return current_function_.has_value();
}

void LambdaFilter::lambdafy() {
  static std::list<LowerCaseString> headers;

  const auto &current_function = current_function_.value();

  const std::string &invocation_type = current_function.async_
                                           ? INVOCATION_TYPE_EVENT
                                           : INVOCATION_TYPE_REQ_RESP;
  headers.push_back(INVOCATION_TYPE);
  request_headers_->addReference(INVOCATION_TYPE, invocation_type);

  headers.push_back(LOG_TYPE);
  request_headers_->addReference(LOG_TYPE, LOG_NONE);

  // TOOO(yuval-k) constify this and change the header list to
  // ref-or-inline like in header map
  headers.push_back(LowerCaseString("host"));
  request_headers_->insertHost().value(*current_function.host_);

  headers.push_back(LowerCaseString("content-type"));

  aws_authenticator_.sign(request_headers_, std::move(headers),
                          *current_function.region_);
  cleanup();
}

void LambdaFilter::cleanup() {
  request_headers_ = nullptr;
  current_function_ = absl::optional<Function>();
}

} // namespace Http
} // namespace Envoy
