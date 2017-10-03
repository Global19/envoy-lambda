#include <string>
#include <algorithm>
#include <vector>
#include <list>

#include "http_filter.h"

#include "server/config/network/http_connection_manager.h"

#include "envoy/http/header_map.h"

#include "common/common/hex.h"
#include "common/common/empty_string.h"
#include "common/common/utility.h"

namespace Solo {
namespace Lambda {

LambdaFilter::LambdaFilter(std::string access_key, std::string secret_key, ClusterFunctionMap functions) : 
  functions_(std::move(functions)),
  active_(false), 
  awsAuthenticator_(std::move(access_key), std::move(secret_key), std::move(std::string("lambda"))) {
}

LambdaFilter::~LambdaFilter() {}

void LambdaFilter::onDestroy() {}

std::string LambdaFilter::functionUrlPath() {

  std::stringstream val;
  val << "/2015-03-31/functions/"<< currentFunction_.func_name_ << "/invocations";
  return val.str();
}

FilterHeadersStatus LambdaFilter::decodeHeaders(HeaderMap& headers, bool end_stream) {

  const Envoy::Router::RouteEntry* routeEntry = decoder_callbacks_->route()->routeEntry();

  if (routeEntry == nullptr) {
    return FilterHeadersStatus::Continue;    
  }

  const std::string& cluster_name = routeEntry->clusterName();
  ClusterFunctionMap::iterator currentFunction = functions_.find(cluster_name);
  if (currentFunction == functions_.end()){
    return FilterHeadersStatus::Continue;    
  }

  active_ = true;
  currentFunction_ = currentFunction->second;

  headers.insertMethod().value().setReference(Http::Headers::get().MethodValues.Post);
  
//  headers.removeContentLength();  
  headers.insertPath().value(functionUrlPath());
  request_headers_ = &headers;
  
  
  ENVOY_LOG(debug, "decodeHeaders called end = {}", end_stream);
  

  return FilterHeadersStatus::StopIteration;
}

FilterDataStatus LambdaFilter::decodeData(Buffer::Instance& data, bool end_stream) {

  if (!active_) {
    return FilterDataStatus::Continue;    
  }
  // calc hash of data
  ENVOY_LOG(debug, "decodeData called end = {} data = {}", end_stream, data.length());
  
  awsAuthenticator_.update_payload_hash(data);

  if (end_stream) {

    lambdafy();
    // Authorization: AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/iam/aws4_request, SignedHeaders=content-type;host;x-amz-date, Signature=5d672d79c15b13162d9279b0855cfba6789a8edb4c82c400e06b5924a6f2b5d7
    request_headers_ = nullptr;
    active_ = false;
    // add header ?!
    // get stream id
    return FilterDataStatus::Continue;
  }

  return FilterDataStatus::StopIterationAndBuffer;
}

void LambdaFilter::lambdafy() {
  std::list<LowerCaseString> headers;

  headers.push_back(LowerCaseString("x-amz-invocation-type"));
  request_headers_->addCopy(LowerCaseString("x-amz-invocation-type"), std::string("RequestResponse"));
  
//  headers.push_back(LowerCaseString("x-amz-client-context"));
//  request_headers_->addCopy(LowerCaseString("x-amz-client-context"), std::string(""));

  headers.push_back(LowerCaseString("x-amz-log-type"));
  request_headers_->addCopy(LowerCaseString("x-amz-log-type"), std::string("None"));
  
  headers.push_back(LowerCaseString("host"));
  request_headers_->insertHost().value(currentFunction_.hostname_);

  headers.push_back(LowerCaseString("content-type"));
    
  awsAuthenticator_.sign(request_headers_, std::move(headers), currentFunction_.region_);
}

FilterTrailersStatus LambdaFilter::decodeTrailers(HeaderMap& headers) {
  if (!active_) {
    return FilterTrailersStatus::Continue;    
  }

  lambdafy();
  
  return FilterTrailersStatus::Continue;
}

void LambdaFilter::setDecoderFilterCallbacks(StreamDecoderFilterCallbacks& callbacks) {
  decoder_callbacks_ = &callbacks;
}

} // Lambda
} // Solo