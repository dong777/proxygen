/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <folly/Foreach.h>
#include <folly/experimental/wangle/ConnectionManager.h>
#include <folly/io/Cursor.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/TimeoutManager.h>
#include <gtest/gtest.h>
#include <proxygen/lib/http/codec/test/MockHTTPCodec.h>
#include <proxygen/lib/http/codec/test/TestUtils.h>
#include <proxygen/lib/http/session/HTTPDirectResponseHandler.h>
#include <proxygen/lib/http/session/HTTPDownstreamSession.h>
#include <proxygen/lib/http/session/HTTPSession.h>
#include <proxygen/lib/http/session/test/HTTPSessionMocks.h>
#include <proxygen/lib/http/session/test/HTTPSessionTest.h>
#include <proxygen/lib/http/session/test/TestUtils.h>
#include <proxygen/lib/test/TestAsyncTransport.h>
#include <string>
#include <strstream>
#include <thrift/lib/cpp/test/MockTAsyncTransport.h>
#include <vector>

using namespace apache::thrift::async;
using namespace apache::thrift::test;
using namespace apache::thrift::transport;
using namespace folly::wangle;
using namespace folly;
using namespace proxygen;
using namespace std;
using namespace testing;

const HTTPSettings kDefaultIngressSettings{
  { SettingsId::INITIAL_WINDOW_SIZE, 65536 }
};

class MockCodecDownstreamTest: public testing::Test {
 public:
  MockCodecDownstreamTest()
    : eventBase_(),
      codec_(new StrictMock<MockHTTPCodec>()),
      transport_(new NiceMock<MockTAsyncTransport>()),
      transactionTimeouts_(makeInternalTimeoutSet(&eventBase_)) {

    EXPECT_CALL(*transport_, good())
      .WillRepeatedly(ReturnPointee(&transportGood_));
    EXPECT_CALL(*transport_, closeNow())
      .WillRepeatedly(Assign(&transportGood_, false));
    EXPECT_CALL(*transport_, getEventBase())
      .WillRepeatedly(Return(&eventBase_));
    EXPECT_CALL(*transport_, setReadCallback(_))
      .WillRepeatedly(SaveArg<0>(&transportCb_));
    EXPECT_CALL(mockController_, attachSession(_));
    EXPECT_CALL(*codec_, setCallback(_))
      .WillRepeatedly(SaveArg<0>(&codecCallback_));
    EXPECT_CALL(*codec_, supportsParallelRequests())
      .WillRepeatedly(Return(true));
    EXPECT_CALL(*codec_, supportsPushTransactions())
      .WillRepeatedly(Return(true));
    EXPECT_CALL(*codec_, getTransportDirection())
      .WillRepeatedly(Return(TransportDirection::DOWNSTREAM));
    EXPECT_CALL(*codec_, getEgressSettings());
    EXPECT_CALL(*codec_, supportsStreamFlowControl())
      .WillRepeatedly(Return(true));
    EXPECT_CALL(*codec_, setParserPaused(_))
      .WillRepeatedly(Return());
    EXPECT_CALL(*codec_, supportsSessionFlowControl())
      .WillRepeatedly(Return(true)); // simulate spdy 3.1
    EXPECT_CALL(*codec_, getIngressSettings())
      .WillRepeatedly(Return(&kDefaultIngressSettings));
    EXPECT_CALL(*codec_, isReusable())
      .WillRepeatedly(ReturnPointee(&reusable_));
    EXPECT_CALL(*codec_, isWaitingToDrain())
      .WillRepeatedly(ReturnPointee(&drainPending_));
    EXPECT_CALL(*codec_, generateSettings(_));
    EXPECT_CALL(*codec_, createStream())
      .WillRepeatedly(InvokeWithoutArgs([&] {
            return pushStreamID_ += 2;
          }));
    EXPECT_CALL(*codec_, enableDoubleGoawayDrain())
      .WillRepeatedly(Invoke([&] { doubleGoaway_ = true; }));
    EXPECT_CALL(*codec_, generateGoaway(_, _, _))
      .WillRepeatedly(Invoke([this] (IOBufQueue& writeBuf,
                                     HTTPCodec::StreamID lastStream,
                                     ErrorCode code) {
            if (reusable_) {
              reusable_ = false;
              drainPending_ = doubleGoaway_;
            } else if (!drainPending_) {
              return 0;
            } else {
              drainPending_ = false;
            }
            if (liveGoaways_) {
              writeBuf.append(string("x"));
            }
            return 1;
          }));
    EXPECT_CALL(*codec_, generateRstStream(_, _, _))
      .WillRepeatedly(Return(1));

    httpSession_ = new HTTPDownstreamSession(
      transactionTimeouts_.get(),
      std::move(TAsyncTransport::UniquePtr(transport_)),
      localAddr, peerAddr,
      &mockController_,
      std::unique_ptr<HTTPCodec>(codec_),
      mockTransportInfo);
    httpSession_->startNow();
    eventBase_.loop();
  }

  // Pass a function to execute inside Codec::onIngress(). This function also
  // takes care of passing an empty ingress buffer to the codec.
  template<class T>
  void onIngressImpl(T f) {
    EXPECT_CALL(*codec_, onIngress(_))
      .WillOnce(Invoke(f));

    void* buf;
    size_t bufSize;
    transportCb_->getReadBuffer(&buf, &bufSize);
    transportCb_->readDataAvailable(bufSize);
  }

  void testGoaway(bool doubleGoaway, bool dropConnection);

 protected:

  EventBase eventBase_;
  // invalid once httpSession_ is destroyed
  StrictMock<MockHTTPCodec>* codec_;
  HTTPCodec::Callback* codecCallback_{nullptr};
  NiceMock<MockTAsyncTransport>* transport_;
  TAsyncTransport::ReadCallback* transportCb_;
  TAsyncTimeoutSet::UniquePtr transactionTimeouts_;
  StrictMock<MockController> mockController_;
  HTTPDownstreamSession* httpSession_;
  HTTPCodec::StreamID pushStreamID_{0};
  bool reusable_{true};
  bool transportGood_{true};
  bool drainPending_{false};
  bool doubleGoaway_{false};
  bool liveGoaways_{false};
};

TEST_F(MockCodecDownstreamTest, on_abort_then_timeouts) {
  // Test what happens when txn1 (out of many transactions) gets an abort
  // followed by a transaction timeout followed by a write timeout
  MockHTTPHandler handler1;
  MockHTTPHandler handler2;
  auto req1 = makeGetRequest();
  auto req2 = makeGetRequest();

  fakeMockCodec(*codec_);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1))
    .WillOnce(Return(&handler2));

  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(Invoke([&handler1] (HTTPTransaction* txn) {
          handler1.txn_ = txn; }));
  EXPECT_CALL(handler1, onHeadersComplete(_))
    .WillOnce(Invoke([&handler1] (std::shared_ptr<HTTPMessage> msg) {
          handler1.sendHeaders(200, 100);
          handler1.sendBody(100);
        }));
  EXPECT_CALL(handler1, onEgressPaused());
  EXPECT_CALL(handler1, onError(_)).Times(2);
  EXPECT_CALL(handler1, detachTransaction());
  EXPECT_CALL(handler2, setTransaction(_))
    .WillOnce(Invoke([&handler2] (HTTPTransaction* txn) {
          handler2.txn_ = txn; }));
  EXPECT_CALL(handler2, onHeadersComplete(_))
    .WillOnce(Invoke([&handler2] (std::shared_ptr<HTTPMessage> msg) {
          handler2.sendHeaders(200, 100);
          handler2.sendBody(100);
        }));
  EXPECT_CALL(handler2, onEgressPaused());
  EXPECT_CALL(*transport_, writeChain(_, _, _));
  EXPECT_CALL(handler2, onError(_))
    .WillOnce(Invoke([&] (const HTTPException& ex) {
          ASSERT_EQ(ex.getProxygenError(), kErrorWriteTimeout);
        }));
  EXPECT_CALL(handler2, detachTransaction());

  EXPECT_CALL(mockController_, detachSession(_));

  codecCallback_->onMessageBegin(HTTPCodec::StreamID(1), req1.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(1), std::move(req1));
  codecCallback_->onMessageBegin(HTTPCodec::StreamID(3), req2.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(3), std::move(req2));
  // do the write, enqeue byte event
  eventBase_.loop();

  // recv an abort, detach the handler from txn1 (txn1 stays around due to the
  // enqueued byte event)
  codecCallback_->onAbort(HTTPCodec::StreamID(1), ErrorCode::PROTOCOL_ERROR);
  // recv a transaction timeout on txn1 (used to erroneously create a direct
  // response handler)
  handler1.txn_->timeoutExpired();

  // have a write timeout expire (used to cause the direct response handler to
  // write out data, messing up the state machine)
  httpSession_->shutdownTransportWithReset(kErrorWriteTimeout);
  eventBase_.loop();
}

TEST_F(MockCodecDownstreamTest, server_push) {
  MockHTTPHandler handler;
  MockHTTPPushHandler pushHandler;
  auto req = makeGetRequest();
  HTTPTransaction* pushTxn = nullptr;

  InSequence enforceOrder;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));
  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler.txn_));

  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          pushTxn = handler.txn_->newPushedTransaction(
            &pushHandler, handler.txn_->getPriority());
          pushHandler.sendPushHeaders("/foo", "www.foo.com", 100);
          pushHandler.sendBody(100);
          pushTxn->sendEOM();
          eventBase_.loop(); // flush the push txn's body
        }));
  EXPECT_CALL(pushHandler, setTransaction(_))
    .WillOnce(Invoke([&pushHandler] (HTTPTransaction* txn) {
          pushHandler.txn_ = txn; }));

  EXPECT_CALL(*codec_, generateHeader(_, 2, _, _, _));
  EXPECT_CALL(*codec_, generateBody(_, 2, PtrBufHasLen(100), true));
  EXPECT_CALL(pushHandler, detachTransaction());

  EXPECT_CALL(handler, onEOM())
    .WillOnce(Invoke([&] {
          handler.sendReplyWithBody(200, 100);
          eventBase_.loop(); // flush the response to the normal request
        }));

  EXPECT_CALL(*codec_, generateHeader(_, 1, _, _, _));
  EXPECT_CALL(*codec_, generateBody(_, 1, PtrBufHasLen(100), true));
  EXPECT_CALL(handler, detachTransaction());

  codecCallback_->onMessageBegin(HTTPCodec::StreamID(1), req.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(1), std::move(req));
  codecCallback_->onMessageComplete(HTTPCodec::StreamID(1), false);

  EXPECT_CALL(mockController_, detachSession(_));
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
}

TEST_F(MockCodecDownstreamTest, server_push_after_goaway) {
  // Tests if goaway
  //   - drains acknowledged server push transactions
  //   - aborts server pushed transactions not created at the client
  //   - prevents new transactions from being created.
  MockHTTPHandler handler;
  MockHTTPPushHandler pushHandler1;
  MockHTTPPushHandler pushHandler2;
  HTTPTransaction* pushTxn = nullptr;

  fakeMockCodec(*codec_);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));

  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(Invoke([&handler] (HTTPTransaction* txn) {
          handler.txn_ = txn; }));
  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          // Initiate server push transactions.
          pushTxn = handler.txn_->newPushedTransaction(
            &pushHandler1, handler.txn_->getPriority());
          CHECK(pushTxn->getID() == HTTPCodec::StreamID(2));
          pushHandler1.sendPushHeaders("/foo", "www.foo.com", 100);
          pushHandler1.sendBody(100);
          pushTxn->sendEOM();
          // Initiate the second push transaction which will be aborted
          pushTxn = handler.txn_->newPushedTransaction(
            &pushHandler2, handler.txn_->getPriority());
          CHECK(pushTxn->getID() == HTTPCodec::StreamID(4));
          pushHandler2.sendPushHeaders("/foo", "www.foo.com", 100);
          pushHandler2.sendBody(100);
          pushTxn->sendEOM();
        }));
  // Push transaction 1 - drained
  EXPECT_CALL(pushHandler1, setTransaction(_))
    .WillOnce(Invoke([&pushHandler1] (HTTPTransaction* txn) {
          pushHandler1.txn_ = txn; }));
  EXPECT_CALL(pushHandler1, detachTransaction());
  // Push transaction 2 - aborted by onError after goaway
  EXPECT_CALL(pushHandler2, setTransaction(_))
    .WillOnce(Invoke([&pushHandler2] (HTTPTransaction* txn) {
          pushHandler2.txn_ = txn; }));
  EXPECT_CALL(pushHandler2, onError(_))
    .WillOnce(Invoke([&] (const HTTPException& err) {
          EXPECT_TRUE(err.hasProxygenError());
          EXPECT_EQ(err.getProxygenError(), kErrorStreamUnacknowledged);
        }));
  EXPECT_CALL(pushHandler2, detachTransaction());

  EXPECT_CALL(handler, onEOM());
  EXPECT_CALL(handler, detachTransaction());

  // Receive client request
  auto req = makeGetRequest();
  codecCallback_->onMessageBegin(HTTPCodec::StreamID(1), req.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(1), std::move(req));
  codecCallback_->onMessageComplete(HTTPCodec::StreamID(1), false);

  // Receive goaway acknowledging only the first pushed transactions with id 2.
  codecCallback_->onGoaway(2, ErrorCode::NO_ERROR);

  // New server pushed transaction cannot be created after goaway
  MockHTTPPushHandler pushHandler3;
  EXPECT_EQ(handler.txn_->newPushedTransaction(&pushHandler3,
        handler.txn_->getPriority()), nullptr);

  // Send response to the initial client request and this destroys the session
  handler.sendReplyWithBody(200, 100);

  eventBase_.loop();

  EXPECT_CALL(mockController_, detachSession(_));
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
}

TEST_F(MockCodecDownstreamTest, server_push_abort) {
  // Test that assoc txn and other push txns are not affected when client aborts
  // a push txn
  MockHTTPHandler handler;
  MockHTTPPushHandler pushHandler1;
  MockHTTPPushHandler pushHandler2;
  HTTPTransaction* pushTxn1 = nullptr;
  HTTPTransaction* pushTxn2 = nullptr;

  fakeMockCodec(*codec_);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));

  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(Invoke([&handler] (HTTPTransaction* txn) {
          handler.txn_ = txn; }));
  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          // Initiate server push transactions
          pushTxn1 = handler.txn_->newPushedTransaction(
            &pushHandler1, handler.txn_->getPriority());
          CHECK(pushTxn1->getID() == HTTPCodec::StreamID(2));
          pushHandler1.sendPushHeaders("/foo", "www.foo.com", 100);
          pushHandler1.sendBody(100);

          pushTxn2 = handler.txn_->newPushedTransaction(
            &pushHandler2, handler.txn_->getPriority());
          CHECK(pushTxn2->getID() == HTTPCodec::StreamID(4));
          pushHandler2.sendPushHeaders("/bar", "www.bar.com", 200);
          pushHandler2.sendBody(200);
          pushTxn2->sendEOM();
        }));

  // pushTxn1 should be aborted
  EXPECT_CALL(pushHandler1, setTransaction(_))
    .WillOnce(Invoke([&pushHandler1] (HTTPTransaction* txn) {
          pushHandler1.txn_ = txn; }));
  EXPECT_CALL(pushHandler1, onError(_))
    .WillOnce(Invoke([&] (const HTTPException& err) {
          EXPECT_TRUE(err.hasProxygenError());
          EXPECT_EQ(err.getProxygenError(), kErrorStreamAbort);
        }));
  EXPECT_CALL(pushHandler1, detachTransaction());

  EXPECT_CALL(pushHandler2, setTransaction(_))
    .WillOnce(Invoke([&pushHandler2] (HTTPTransaction* txn) {
          pushHandler2.txn_ = txn; }));
  EXPECT_CALL(pushHandler2, detachTransaction());

  EXPECT_CALL(handler, onEOM());
  EXPECT_CALL(handler, detachTransaction());

  // Receive client request
  auto req = makeGetRequest();
  codecCallback_->onMessageBegin(HTTPCodec::StreamID(1), req.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(1), std::move(req));
  codecCallback_->onMessageComplete(HTTPCodec::StreamID(1), false);

  // Send client abort on one push txn
  codecCallback_->onAbort(HTTPCodec::StreamID(2), ErrorCode::CANCEL);

  handler.sendReplyWithBody(200, 100);

  eventBase_.loop();

  EXPECT_CALL(mockController_, detachSession(_));
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
}

TEST_F(MockCodecDownstreamTest, server_push_abort_assoc) {
  // Test that all associated push transactions are aborted when client aborts
  // the assoc stream
  MockHTTPHandler handler;
  MockHTTPPushHandler pushHandler1;
  MockHTTPPushHandler pushHandler2;

  fakeMockCodec(*codec_);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));

  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(Invoke([&handler] (HTTPTransaction* txn) {
          handler.txn_ = txn; }));
  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          // Initiate server push transactions
          auto pushTxn = handler.txn_->newPushedTransaction(
            &pushHandler1, handler.txn_->getPriority());
          CHECK(pushTxn->getID() == HTTPCodec::StreamID(2));
          pushHandler1.sendPushHeaders("/foo", "www.foo.com", 100);
          pushHandler1.sendBody(100);
          eventBase_.loop();

          pushTxn = handler.txn_->newPushedTransaction(
            &pushHandler2, handler.txn_->getPriority());
          CHECK(pushTxn->getID() == HTTPCodec::StreamID(4));
          pushHandler2.sendPushHeaders("/foo", "www.foo.com", 100);
          pushHandler2.sendBody(100);
          eventBase_.loop();
        }));

  // Both push txns and the assoc txn should be aborted
  EXPECT_CALL(pushHandler1, setTransaction(_))
    .WillOnce(Invoke([&pushHandler1] (HTTPTransaction* txn) {
          pushHandler1.txn_ = txn; }));
  EXPECT_CALL(pushHandler1, onError(_))
    .WillOnce(Invoke([&] (const HTTPException& err) {
          EXPECT_TRUE(err.hasProxygenError());
          EXPECT_EQ(err.getProxygenError(), kErrorStreamAbort);
        }));
  EXPECT_CALL(pushHandler1, detachTransaction());

  EXPECT_CALL(pushHandler2, setTransaction(_))
    .WillOnce(Invoke([&pushHandler2] (HTTPTransaction* txn) {
          pushHandler2.txn_ = txn; }));
  EXPECT_CALL(pushHandler2, onError(_))
    .WillOnce(Invoke([&] (const HTTPException& err) {
          EXPECT_TRUE(err.hasProxygenError());
          EXPECT_EQ(err.getProxygenError(), kErrorStreamAbort);
        }));
  EXPECT_CALL(pushHandler2, detachTransaction());

  EXPECT_CALL(handler, onError(_))
    .WillOnce(Invoke([&] (const HTTPException& err) {
          EXPECT_TRUE(err.hasProxygenError());
          EXPECT_EQ(err.getProxygenError(), kErrorStreamAbort);
        }));
  EXPECT_CALL(handler, detachTransaction());

  // Receive client request
  auto req = makeGetRequest();
  codecCallback_->onMessageBegin(HTTPCodec::StreamID(1), req.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(1), std::move(req));

  // Send client abort on assoc stream
  codecCallback_->onAbort(HTTPCodec::StreamID(1), ErrorCode::CANCEL);

  eventBase_.loop();

  EXPECT_CALL(mockController_, detachSession(_));
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
}

TEST_F(MockCodecDownstreamTest, server_push_client_message) {
  // Test that error is generated when client sends data on a pushed stream
  MockHTTPHandler handler;
  MockHTTPPushHandler pushHandler;
  auto req = makeGetRequest();
  HTTPTransaction* pushTxn = nullptr;

  InSequence enforceOrder;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));
  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler.txn_));

  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          pushTxn = handler.txn_->newPushedTransaction(
            &pushHandler, handler.txn_->getPriority());
        }));
  EXPECT_CALL(pushHandler, setTransaction(_))
    .WillOnce(Invoke([&pushHandler] (HTTPTransaction* txn) {
          pushHandler.txn_ = txn; }));

  codecCallback_->onMessageBegin(HTTPCodec::StreamID(1), req.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(1), std::move(req));

  EXPECT_CALL(*codec_, generateRstStream(_, 2, ErrorCode::STREAM_CLOSED))
    .WillRepeatedly(Return(1));
  EXPECT_CALL(pushHandler, onError(_))
    .WillOnce(Invoke([&] (const HTTPException& ex) {
          EXPECT_TRUE(ex.hasCodecStatusCode());
          EXPECT_EQ(ex.getCodecStatusCode(), ErrorCode::STREAM_CLOSED);
        }));
  EXPECT_CALL(pushHandler, detachTransaction());

  // While the assoc stream is open and pushHandler has been initialized, send
  // an upstream message on the push stream causing a RST_STREAM.
  req = makeGetRequest();
  codecCallback_->onMessageBegin(HTTPCodec::StreamID(2), req.get());

  EXPECT_CALL(handler, onEOM())
    .WillOnce(InvokeWithoutArgs([&] {
          handler.sendReplyWithBody(200, 100);
          eventBase_.loop(); // flush the response to the assoc request
        }));
  EXPECT_CALL(*codec_, generateHeader(_, 1, _, _, _));
  EXPECT_CALL(*codec_, generateBody(_, 1, PtrBufHasLen(100), true));
  EXPECT_CALL(handler, detachTransaction());

  // Complete the assoc request/response
  codecCallback_->onMessageComplete(HTTPCodec::StreamID(1), false);

  eventBase_.loop();

  EXPECT_CALL(mockController_, detachSession(_));
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
}

TEST_F(MockCodecDownstreamTest, read_timeout) {
  // Test read timeout path
  MockHTTPHandler handler1;
  auto req1 = makeGetRequest();

  fakeMockCodec(*codec_);
  EXPECT_CALL(*codec_, onIngressEOF())
    .WillRepeatedly(Return());

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));

  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(Invoke([&handler1] (HTTPTransaction* txn) {
          handler1.txn_ = txn; }));
  EXPECT_CALL(handler1, onHeadersComplete(_));

  codecCallback_->onMessageBegin(HTTPCodec::StreamID(1), req1.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(1), std::move(req1));
  // force the read timeout to expire, should be a no-op because the txn is
  // still expecting EOM and has its own timer.
  httpSession_->timeoutExpired();
  EXPECT_EQ(httpSession_->getConnectionCloseReason(),
            ConnectionCloseReason::kMAX_REASON);

  EXPECT_CALL(handler1, onEOM())
    .WillOnce(Invoke([&handler1] () {
          handler1.txn_->pauseIngress();
        }));

  // send the EOM, then another timeout.  Still no-op since it's waiting
  // upstream
  codecCallback_->onMessageComplete(HTTPCodec::StreamID(1), false);
  httpSession_->timeoutExpired();
  EXPECT_EQ(httpSession_->getConnectionCloseReason(),
            ConnectionCloseReason::kMAX_REASON);

  EXPECT_CALL(*transport_, writeChain(_, _, _))
    .WillRepeatedly(Invoke([] (TAsyncTransport::WriteCallback* callback,
                               std::shared_ptr<folly::IOBuf> iob,
                               apache::thrift::async::WriteFlags flags) {
                             callback->writeSuccess();
                           }));

  EXPECT_CALL(handler1, detachTransaction());

  // Send the response, timeout.  Now it's idle and should close.
  handler1.txn_->resumeIngress();
  handler1.sendReplyWithBody(200, 100);
  eventBase_.loop();

  httpSession_->timeoutExpired();
  EXPECT_EQ(httpSession_->getConnectionCloseReason(),
            ConnectionCloseReason::TIMEOUT);

  // tear down the test
  EXPECT_CALL(mockController_, detachSession(_));
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
}

TEST_F(MockCodecDownstreamTest, ping) {
  // Test ping mechanism and that we prioritize the ping reply
  MockHTTPHandler handler1;
  auto req1 = makeGetRequest();

  InSequence enforceOrder;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));

  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(Invoke([&handler1] (HTTPTransaction* txn) {
          handler1.txn_ = txn; }));
  EXPECT_CALL(handler1, onHeadersComplete(_));
  EXPECT_CALL(handler1, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler1] () {
          handler1.sendReplyWithBody(200, 100);
        }));

  // Header egresses immediately
  EXPECT_CALL(*codec_, generateHeader(_, _, _, _, _));
  // Ping jumps ahead of queued body in the loop callback
  EXPECT_CALL(*codec_, generatePingReply(_, _));
  EXPECT_CALL(*codec_, generateBody(_, _, _, true));
  EXPECT_CALL(handler1, detachTransaction());

  codecCallback_->onMessageBegin(HTTPCodec::StreamID(1), req1.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(1), std::move(req1));
  codecCallback_->onMessageComplete(HTTPCodec::StreamID(1), false);
  codecCallback_->onPingRequest(1);

  eventBase_.loop();

  //EXPECT_CALL(*codec_, onIngressEOF());
  EXPECT_CALL(mockController_, detachSession(_));
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
}

TEST_F(MockCodecDownstreamTest, buffering) {
  StrictMock<MockHTTPHandler> handler;
  auto req1 = makePostRequest();
  auto chunk = makeBuf(10);
  auto chunkStr = chunk->clone()->moveToFbString();

  fakeMockCodec(*codec_);

  httpSession_->setDefaultReadBufferLimit(10);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));

  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(Invoke([&handler] (HTTPTransaction* txn) {
          handler.txn_ = txn; }));
  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(InvokeWithoutArgs([&handler] () {
          handler.txn_->pauseIngress();
        }));

  EXPECT_CALL(*transport_, writeChain(_, _, _))
    .WillRepeatedly(Invoke([&] (TAsyncTransport::WriteCallback* callback,
                                const shared_ptr<IOBuf> iob,
                                WriteFlags flags) {
                             callback->writeSuccess();
                           }));

  codecCallback_->onMessageBegin(HTTPCodec::StreamID(1), req1.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(1), std::move(req1));
  for (int i = 0; i < 2; i++) {
    codecCallback_->onBody(HTTPCodec::StreamID(1), chunk->clone());
  }
  codecCallback_->onMessageComplete(HTTPCodec::StreamID(1), false);

  EXPECT_CALL(handler, onBody(_))
    .WillOnce(ExpectString(chunkStr))
    .WillOnce(ExpectString(chunkStr));

  EXPECT_CALL(handler, onEOM());

  EXPECT_CALL(handler, detachTransaction());

  eventBase_.runAfterDelay([&handler, this] {
      handler.txn_->resumeIngress();
      handler.sendReplyWithBody(200, 100);
    }, 30);
  eventBase_.runAfterDelay([&handler, this] {
      httpSession_->shutdownTransportWithReset(
        ProxygenError::kErrorConnectionReset);
    }, 50);

  EXPECT_CALL(mockController_, detachSession(_));
  eventBase_.loop();
}

TEST_F(MockCodecDownstreamTest, spdy_window) {
  // Test window updates
  MockHTTPHandler handler1;
  auto req1 = makeGetRequest();

  fakeMockCodec(*codec_);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));

  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(Invoke([&handler1] (HTTPTransaction* txn) {
          handler1.txn_ = txn; }));
  EXPECT_CALL(handler1, onHeadersComplete(_))
    .WillOnce(InvokeWithoutArgs([this] () {
          codecCallback_->onSettings(
            {{SettingsId::INITIAL_WINDOW_SIZE, 4000}});
        }));
  EXPECT_CALL(handler1, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler1] () {
          handler1.sendHeaders(200, 16000);
          handler1.sendBody(12000);
          // 12kb buffered -> pause upstream
        }));
  EXPECT_CALL(handler1, onEgressPaused())
    .WillOnce(InvokeWithoutArgs([&handler1, this] () {
          eventBase_.runInLoop([this] {
              codecCallback_->onWindowUpdate(1, 4000);
            });
          // triggers 4k send, 8k buffered, resume
        }))
    .WillOnce(InvokeWithoutArgs([&handler1, this] () {
          eventBase_.runInLoop([this] {
              codecCallback_->onWindowUpdate(1, 8000);
            });
          // triggers 8kb send
        }))
    .WillOnce(InvokeWithoutArgs([] () {}));
  EXPECT_CALL(handler1, onEgressResumed())
    .WillOnce(InvokeWithoutArgs([&handler1, this] () {
          handler1.sendBody(4000);
          // 12kb buffered -> pause upstream
        }))
    .WillOnce(InvokeWithoutArgs([&handler1, this] () {
          handler1.txn_->sendEOM();
          eventBase_.runInLoop([this] {
              codecCallback_->onWindowUpdate(1, 4000);
            });
        }));

  EXPECT_CALL(handler1, detachTransaction());

  codecCallback_->onMessageBegin(HTTPCodec::StreamID(1), req1.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(1), std::move(req1));
  codecCallback_->onMessageComplete(HTTPCodec::StreamID(1), false);
  // Pad coverage numbers
  std::ostrstream stream;
  stream << *handler1.txn_ << httpSession_
         << httpSession_->getLocalAddress() << httpSession_->getPeerAddress();
  EXPECT_TRUE(httpSession_->isBusy());

  EXPECT_CALL(mockController_, detachSession(_));

  EXPECT_CALL(*transport_, writeChain(_, _, _))
    .WillRepeatedly(Invoke([] (TAsyncTransport::WriteCallback* callback,
                               std::shared_ptr<folly::IOBuf> iob,
                               apache::thrift::async::WriteFlags flags) {
                             callback->writeSuccess();
                           }));
  eventBase_.loop();
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
}

TEST_F(MockCodecDownstreamTest, double_resume) {
  // Test spdy ping mechanism and egress re-ordering
  MockHTTPHandler handler1;
  auto req1 = makePostRequest();
  auto buf = makeBuf(5);
  auto bufStr = buf->clone()->moveToFbString();

  fakeMockCodec(*codec_);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));

  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(Invoke([&handler1] (HTTPTransaction* txn) {
          handler1.txn_ = txn; }));
  EXPECT_CALL(handler1, onHeadersComplete(_))
    .WillOnce(InvokeWithoutArgs([&handler1, this] {
          handler1.txn_->pauseIngress();
          eventBase_.runAfterDelay([&handler1] {
              handler1.txn_->resumeIngress();
            }, 50);
        }));
  EXPECT_CALL(handler1, onBody(_))
    .WillOnce(Invoke([&handler1, &bufStr] (
                       std::shared_ptr<folly::IOBuf> chain) {
          EXPECT_EQ(bufStr, chain->moveToFbString());
          handler1.txn_->pauseIngress();
          handler1.txn_->resumeIngress();
        }));

  EXPECT_CALL(handler1, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler1] () {
          handler1.sendReplyWithBody(200, 100, false);
        }));
  EXPECT_CALL(handler1, detachTransaction());

  codecCallback_->onMessageBegin(HTTPCodec::StreamID(1), req1.get());
  codecCallback_->onHeadersComplete(HTTPCodec::StreamID(1), std::move(req1));
  codecCallback_->onBody(HTTPCodec::StreamID(1), std::move(buf));
  codecCallback_->onMessageComplete(HTTPCodec::StreamID(1), false);

  EXPECT_CALL(mockController_, detachSession(_));

  EXPECT_CALL(*transport_, writeChain(_, _, _))
    .WillRepeatedly(Invoke([] (TAsyncTransport::WriteCallback* callback,
                               std::shared_ptr<folly::IOBuf> iob,
                               apache::thrift::async::WriteFlags flags) {
                             callback->writeSuccess();
                           }));

  eventBase_.loop();
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
}

TEST(HTTPDownstreamTest, new_txn_egress_paused) {
  // Send 1 request with prio=0
  // Have egress pause while sending the first response
  // Send a second request with prio=1
  //   -- the new txn should start egress paused
  // Finish the body and eom both responses
  // Unpause egress
  // The first txn should complete first
  HTTPCodec::StreamID curId(1);
  std::array<NiceMock<MockHTTPHandler>, 2> handlers;
  TAsyncTransport::WriteCallback* delayedWrite = nullptr;
  EventBase evb;

  // Setup the controller and its expecations.
  NiceMock<MockController> mockController;
  EXPECT_CALL(mockController, getRequestHandler(_, _))
    .WillOnce(Return(&handlers[0]))
    .WillOnce(Return(&handlers[1]));

  // Setup the codec, its callbacks, and its expectations.
  auto codec = makeDownstreamParallelCodec();
  HTTPCodec::Callback* codecCallback = nullptr;
  EXPECT_CALL(*codec, setCallback(_))
    .WillRepeatedly(SaveArg<0>(&codecCallback));
  // Let the codec generate a huge header for the first txn
  static const uint64_t header1Len = HTTPSession::getPendingWriteMax();
  static const uint64_t header2Len = 20;
  static const uint64_t body1Len = 30;
  static const uint64_t body2Len = 40;
  EXPECT_CALL(*codec, generateHeader(_, _, _, _, _))
    .WillOnce(Invoke([&] (IOBufQueue& writeBuf,
                          HTTPCodec::StreamID stream,
                          const HTTPMessage& msg,
                          HTTPCodec::StreamID assocStream,
                          HTTPHeaderSize* size) {
                       CHECK_EQ(stream, HTTPCodec::StreamID(1));
                       writeBuf.append(makeBuf(header1Len));
                       if (size) {
                         size->uncompressed = header1Len;
                       }
                     }))
    // Let the codec generate a regular sized header for the second txn
    .WillOnce(Invoke([&] (IOBufQueue& writeBuf,
                          HTTPCodec::StreamID stream,
                          const HTTPMessage& msg,
                          HTTPCodec::StreamID ascocStream,
                          HTTPHeaderSize* size) {
                       CHECK_EQ(stream, HTTPCodec::StreamID(2));
                       writeBuf.append(makeBuf(header2Len));
                       if (size) {
                         size->uncompressed = header2Len;
                       }
                     }));
  EXPECT_CALL(*codec, generateBody(_, _, _, _))
    .WillOnce(Invoke([&] (IOBufQueue& writeBuf,
                          HTTPCodec::StreamID stream,
                          shared_ptr<IOBuf> chain,
                          bool eom) {
                       CHECK_EQ(stream, HTTPCodec::StreamID(1));
                       CHECK_EQ(chain->computeChainDataLength(), body1Len);
                       CHECK(eom);
                       writeBuf.append(chain->clone());
                       return body1Len;
                     }))
    .WillOnce(Invoke([&] (IOBufQueue& writeBuf,
                          HTTPCodec::StreamID stream,
                          shared_ptr<IOBuf> chain,
                          bool eom) {
                       CHECK_EQ(stream, HTTPCodec::StreamID(2));
                       CHECK_EQ(chain->computeChainDataLength(), body2Len);
                       CHECK(eom);
                       writeBuf.append(chain->clone());
                       return body2Len;
                     }));

  bool transportGood = true;
  auto transport = newMockTransport(&evb);
  EXPECT_CALL(*transport, good())
    .WillRepeatedly(ReturnPointee(&transportGood));
  EXPECT_CALL(*transport, closeNow())
    .WillRepeatedly(Assign(&transportGood, false));
  // We expect the writes to come in this order:
  // txn1 headers -> txn1 eom -> txn2 headers -> txn2 eom
  EXPECT_CALL(*transport, writeChain(_, _, _))
    .WillOnce(Invoke([&] (TAsyncTransport::WriteCallback* callback,
                          const shared_ptr<IOBuf> iob,
                          WriteFlags flags) {
                       CHECK_EQ(iob->computeChainDataLength(), header1Len);
                       delayedWrite = callback;
                       CHECK(delayedWrite != nullptr);
                     }))
    .WillOnce(Invoke([&] (TAsyncTransport::WriteCallback* callback,
                          const shared_ptr<IOBuf> iob,
                          WriteFlags flags) {
                       CHECK(delayedWrite == nullptr);
                       // Make sure the second txn has started
                       CHECK(handlers[1].txn_ != nullptr);
                       // Headers from txn 2 jump the queue and get lumped into
                       // this write
                       CHECK_EQ(iob->computeChainDataLength(),
                                header2Len + body1Len);
                       callback->writeSuccess();
                     }))
    .WillOnce(Invoke([&] (TAsyncTransport::WriteCallback* callback,
                          const shared_ptr<IOBuf> iob,
                          WriteFlags flags) {
                       CHECK_EQ(iob->computeChainDataLength(), body2Len);
                       callback->writeSuccess();
                     }));

  // Create the downstream session, thus initializing codecCallback
  auto transactionTimeouts = makeInternalTimeoutSet(&evb);
  auto session = new HTTPDownstreamSession(
    transactionTimeouts.get(),
    TAsyncTransport::UniquePtr(transport),
    localAddr, peerAddr,
    &mockController, std::move(codec),
    mockTransportInfo);
  session->startNow();

  for (auto& handler: handlers) {
    // Note that order of expecatations doesn't matter here
    EXPECT_CALL(handler, setTransaction(_))
      .WillOnce(SaveArg<0>(&handler.txn_));
    EXPECT_CALL(handler, onHeadersComplete(_))
      .WillOnce(InvokeWithoutArgs([&] {
            CHECK(handler.txn_->isEgressPaused() ==
                  (handler.txn_->getID() == HTTPCodec::StreamID(2)));
          }));
    EXPECT_CALL(handler, onEOM())
      .WillOnce(InvokeWithoutArgs([&] {
            CHECK(handler.txn_->isEgressPaused() ==
                  (handler.txn_->getID() == HTTPCodec::StreamID(2)));
            const HTTPMessage response;
            handler.txn_->sendHeaders(response);
          }));
    EXPECT_CALL(handler, detachTransaction())
      .WillOnce(InvokeWithoutArgs([&] {
            handler.txn_ = nullptr;
          }));
    EXPECT_CALL(handler, onEgressPaused());
  }

  auto p0Msg = getPriorityMessage(0);
  auto p1Msg = getPriorityMessage(1);

  codecCallback->onMessageBegin(curId, p0Msg.get());
  codecCallback->onHeadersComplete(curId, std::move(p0Msg));
  codecCallback->onMessageComplete(curId, false);
  ASSERT_FALSE(handlers[0].txn_->isEgressPaused());
  // looping the evb should pause egress when the huge header gets written out
  evb.loop();
  // Start the second transaction
  codecCallback->onMessageBegin(++curId, p1Msg.get());
  codecCallback->onHeadersComplete(curId, std::move(p1Msg));
  codecCallback->onMessageComplete(curId, false);
  // Make sure both txns have egress paused
  CHECK(handlers[0].txn_ != nullptr);
  ASSERT_TRUE(handlers[0].txn_->isEgressPaused());
  CHECK(handlers[1].txn_ != nullptr);
  ASSERT_TRUE(handlers[1].txn_->isEgressPaused());
  // Send body on the second transaction first, then 1st, but the asserts we
  // have set up check that the first transaction writes out first.
  handlers[1].txn_->sendBody(makeBuf(body2Len));
  handlers[1].txn_->sendEOM();
  handlers[0].txn_->sendBody(makeBuf(body1Len));
  handlers[0].txn_->sendEOM();
  // Now lets ack the first delayed write
  auto tmp = delayedWrite;
  delayedWrite = nullptr;
  tmp->writeSuccess();
  ASSERT_TRUE(handlers[0].txn_ == nullptr);
  ASSERT_TRUE(handlers[1].txn_ == nullptr);

  // Cleanup
  session->shutdownTransport();
  evb.loop();
}

TEST_F(MockCodecDownstreamTest, conn_flow_control_blocked) {
  // Let the connection level flow control window fill and then make sure
  // control frames still can be processed
  InSequence enforceOrder;
  NiceMock<MockHTTPHandler> handler1;
  NiceMock<MockHTTPHandler> handler2;
  auto wantToWrite = spdy::kInitialWindow + 50000;
  auto wantToWriteStr = folly::to<string>(wantToWrite);
  auto req1 = makeGetRequest();
  auto req2 = makeGetRequest();
  auto resp1 = makeResponse(200);
  resp1->getHeaders().set(HTTP_HEADER_CONTENT_LENGTH, wantToWriteStr);
  auto resp2 = makeResponse(200);
  resp2->getHeaders().set(HTTP_HEADER_CONTENT_LENGTH, wantToWriteStr);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));
  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler1.txn_));
  EXPECT_CALL(handler1, onHeadersComplete(_));
  EXPECT_CALL(*codec_, generateHeader(_, 1, _, _, _));
  unsigned bodyLen = 0;
  EXPECT_CALL(*codec_, generateBody(_, 1, _, false))
    .WillRepeatedly(Invoke([&] (folly::IOBufQueue& writeBuf,
                                HTTPCodec::StreamID stream,
                                std::shared_ptr<folly::IOBuf> chain,
                                bool eom) {
                             bodyLen += chain->computeChainDataLength();
                             return 0; // don't want byte events
                           }));

  codecCallback_->onMessageBegin(1, req1.get());
  codecCallback_->onHeadersComplete(1, std::move(req1));
  codecCallback_->onWindowUpdate(1, wantToWrite); // ensure the per-stream
                                                  // window doesn't block
  handler1.txn_->sendHeaders(*resp1);
  handler1.txn_->sendBody(makeBuf(wantToWrite)); // conn blocked, stream open
  handler1.txn_->sendEOM();
  eventBase_.loop(); // actually send (most of) the body
  CHECK_EQ(bodyLen, spdy::kInitialWindow); // should have written a full window

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler2));
  EXPECT_CALL(handler2, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler2.txn_));
  EXPECT_CALL(handler2, onHeadersComplete(_));
  EXPECT_CALL(*codec_, generateHeader(_, 3, _, _, _));

  // Make sure we can send headers of response to a second request
  codecCallback_->onMessageBegin(3, req2.get());
  codecCallback_->onHeadersComplete(3, std::move(req2));
  handler2.txn_->sendHeaders(*resp2);

  eventBase_.loop();

  // Give a connection level window update of 10 bytes -- this should allow 10
  // bytes of the txn1 response to be written
  codecCallback_->onWindowUpdate(0, 10);
  EXPECT_CALL(*codec_, generateBody(_, 1, PtrBufHasLen(10), false));
  eventBase_.loop();

  // Just tear everything down now.
  EXPECT_CALL(handler1, detachTransaction());
  codecCallback_->onAbort(handler1.txn_->getID(), ErrorCode::INTERNAL_ERROR);
  eventBase_.loop();

  EXPECT_CALL(handler2, detachTransaction());
  EXPECT_CALL(mockController_, detachSession(_));
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
  eventBase_.loop();
}

TEST_F(MockCodecDownstreamTest, unpaused_large_post) {
  // Make sure that a large POST that streams into the handler generates
  // connection level flow control so that the entire POST can be received.
  InSequence enforceOrder;
  NiceMock<MockHTTPHandler> handler1;
  unsigned kNumChunks = 10;
  auto wantToWrite = spdy::kInitialWindow * kNumChunks;
  auto wantToWriteStr = folly::to<string>(wantToWrite);
  auto req1 = makePostRequest();
  req1->getHeaders().set(HTTP_HEADER_CONTENT_LENGTH, wantToWriteStr);
  auto req1Body = makeBuf(wantToWrite);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));
  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler1.txn_));

  EXPECT_CALL(handler1, onHeadersComplete(_));
  for (unsigned i = 0; i < kNumChunks; ++i) {
    EXPECT_CALL(*codec_, generateWindowUpdate(_, 0, spdy::kInitialWindow));
    EXPECT_CALL(handler1, onBody(PtrBufHasLen(spdy::kInitialWindow)));
    EXPECT_CALL(*codec_, generateWindowUpdate(_, 1, spdy::kInitialWindow));
  }
  EXPECT_CALL(handler1, onEOM());

  codecCallback_->onMessageBegin(1, req1.get());
  codecCallback_->onHeadersComplete(1, std::move(req1));
  // Give kNumChunks chunks, each of the maximum window size. We should generate
  // window update for each chunk
  for (unsigned i = 0; i < kNumChunks; ++i) {
    codecCallback_->onBody(1, makeBuf(spdy::kInitialWindow));
  }
  codecCallback_->onMessageComplete(1, false);

  // Just tear everything down now.
  EXPECT_CALL(mockController_, detachSession(_));
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
}

TEST_F(MockCodecDownstreamTest, ingress_paused_window_update) {
  // Test sending a large response body while the handler has ingress paused. We
  // should process the ingress window_updates and deliver the full body
  InSequence enforceOrder;
  NiceMock<MockHTTPHandler> handler1;
  auto req = makeGetRequest();
  size_t respSize = spdy::kInitialWindow * 10;
  unique_ptr<HTTPMessage> resp;
  unique_ptr<folly::IOBuf> respBody;
  tie(resp, respBody) = makeResponse(200, respSize);
  size_t written = 0;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));
  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler1.txn_));

  EXPECT_CALL(handler1, onHeadersComplete(_))
    .WillOnce(InvokeWithoutArgs([&] () {
          // Pause ingress. Make sure we process the window updates anyway
          handler1.txn_->pauseIngress();
        }));
  EXPECT_CALL(*codec_, generateHeader(_, _, _, _, _));
  EXPECT_CALL(*codec_, generateBody(_, _, _, _))
    .WillRepeatedly(
      Invoke([&] (folly::IOBufQueue& writeBuf,
                  HTTPCodec::StreamID stream,
                  std::shared_ptr<folly::IOBuf> chain,
                  bool eom) {
               auto len = chain->computeChainDataLength();
               written += len;
               return len;
             }));

  codecCallback_->onWindowUpdate(0, respSize); // open conn-level window
  codecCallback_->onMessageBegin(1, req.get());
  codecCallback_->onHeadersComplete(1, std::move(req));
  EXPECT_TRUE(handler1.txn_->isIngressPaused());

  // Unblock txn-level flow control and try to egress the body
  codecCallback_->onWindowUpdate(1, respSize);
  handler1.txn_->sendHeaders(*resp);
  handler1.txn_->sendBody(std::move(respBody));

  eventBase_.loop();
  EXPECT_EQ(written, respSize);

  // Just tear everything down now.
  EXPECT_CALL(mockController_, detachSession(_));
  httpSession_->shutdownTransportWithReset(kErrorConnectionReset);
}

TEST_F(MockCodecDownstreamTest, shutdown_then_send_push_headers) {
  // Test that notifying session of shutdown before sendHeaders() called on a
  // pushed txn lets that push txn finish.
  EXPECT_CALL(*codec_, supportsPushTransactions())
    .WillRepeatedly(Return(true));

  InSequence enforceOrder;
  NiceMock<MockHTTPHandler> handler;
  MockHTTPPushHandler pushHandler;
  auto req = makeGetRequest();

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));
  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler.txn_));

  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          auto pushTxn = handler.txn_->newPushedTransaction(
            &pushHandler, handler.txn_->getPriority());
          // start shutdown process
          httpSession_->notifyPendingShutdown();
          // we should be able to process new requests
          EXPECT_TRUE(codec_->isReusable());
          pushHandler.sendPushHeaders("/foo", "www.foo.com", 0);
          // we should* still* be able to process new requests
          EXPECT_TRUE(codec_->isReusable());
          pushTxn->sendEOM();
        }));
  EXPECT_CALL(pushHandler, setTransaction(_))
    .WillOnce(SaveArg<0>(&pushHandler.txn_));
  EXPECT_CALL(*codec_, generateHeader(_, 2, _, _, _));
  EXPECT_CALL(*codec_, generateEOM(_, 2));
  EXPECT_CALL(pushHandler, detachTransaction());
  EXPECT_CALL(handler, onEOM())
    .WillOnce(Invoke([&] {
          handler.sendReply();
        }));
  EXPECT_CALL(*codec_, generateHeader(_, 1, _, _, _));
  EXPECT_CALL(*codec_, generateEOM(_, 1));
  EXPECT_CALL(handler, detachTransaction());

  codecCallback_->onMessageBegin(1, req.get());
  codecCallback_->onHeadersComplete(1, std::move(req));
  codecCallback_->onMessageComplete(1, false);

  // finish shutdown
  EXPECT_CALL(*codec_, onIngressEOF());
  EXPECT_CALL(mockController_, detachSession(_));
  httpSession_->dropConnection();

  eventBase_.loop();
}

TEST_F(MockCodecDownstreamTest, read_iobuf_chain_shutdown) {
  // Given an ingress IOBuf chain of 2 parts, if we shutdown after reading the
  // first part of the chain, we shouldn't read the second part.  One way to
  // simulate a 2 part chain is to put more ingress in readBuf while we are
  // inside HTTPCodec::onIngress()

  InSequence enforceOrder;

  auto f = [&] () {
    void* buf;
    size_t bufSize;
    transportCb_->getReadBuffer(&buf, &bufSize);
    transportCb_->readDataAvailable(bufSize);
  };

  EXPECT_CALL(*codec_, onIngress(_))
    .WillOnce(Invoke([&] (const IOBuf& buf) {
          // This first time, don't process any data. This will cause the
          // ingress chain to grow in size later.
          EXPECT_FALSE(buf.isChained());
          return 0;
        }))
    .WillOnce(Invoke([&] (const IOBuf& buf) {
          // Now there should be a second buffer in the chain.
          EXPECT_TRUE(buf.isChained());
          // Shutdown writes. This enough to destroy the session.
          httpSession_->shutdownTransport(false, true);
          return buf.length();
        }));
  // We shouldn't get a third onIngress() callback. This will be enforced by the
  // test framework since the codec is a strict mock.
  EXPECT_CALL(mockController_, detachSession(_));

  f();
  f(); // The first time wasn't processed, so this should make a len=2 chain.
  eventBase_.loop();
}

void MockCodecDownstreamTest::testGoaway(bool doubleGoaway,
                                         bool dropConnection) {
  NiceMock<MockHTTPHandler> handler;
  MockHTTPHandler pushHandler;

  liveGoaways_ = true;
  if (doubleGoaway) {
    EXPECT_CALL(mockController_, getRequestHandler(_, _))
      .WillOnce(Return(&handler));
    EXPECT_CALL(handler, setTransaction(_))
      .WillOnce(SaveArg<0>(&handler.txn_));

    EXPECT_CALL(handler, onHeadersComplete(_));
    EXPECT_CALL(handler, onEOM())
      .WillOnce(Invoke([&] {
            handler.sendReply();
          }));
    EXPECT_CALL(*codec_, generateHeader(_, 1, _, _, _));
    EXPECT_CALL(*codec_, generateEOM(_, 1));
    EXPECT_CALL(handler, detachTransaction());

    // Turn on double GOAWAY drain
    codec_->enableDoubleGoawayDrain();
  }

  // Send a GOAWAY acking uninitiated transactions
  EXPECT_FALSE(drainPending_);
  httpSession_->notifyPendingShutdown();
  EXPECT_EQ(drainPending_, doubleGoaway);
  EXPECT_FALSE(reusable_);

  if (doubleGoaway) {
    // Should be able to process new requests
    auto req1 = makeGetRequest();
    codecCallback_->onMessageBegin(1, req1.get());
    codecCallback_->onHeadersComplete(1, std::move(req1));
    codecCallback_->onMessageComplete(1, false);
  }

  TAsyncTransport::WriteCallback* cb = nullptr;
  EXPECT_CALL(*transport_, writeChain(_, _, _))
    .WillOnce(Invoke([&] (TAsyncTransport::WriteCallback* callback,
                          const shared_ptr<IOBuf> iob,
                          WriteFlags flags) {
                       // don't immediately flush the goaway
                       cb = callback;
                     }));
  if (doubleGoaway || !dropConnection) {
    // single goaway, drop connection doesn't get onIngressEOF
    EXPECT_CALL(*codec_, onIngressEOF());
  }
  eventBase_.loopOnce();

  EXPECT_CALL(mockController_, detachSession(_));
  if (dropConnection) {
    EXPECT_CALL(*transport_, closeNow())
      .WillOnce(DoAll(Assign(&transportGood_, false),
                      Invoke([cb] {
                          cb->writeError(0, TTransportException());
                        })));

    httpSession_->dropConnection();
  } else {
    EXPECT_CALL(*codec_, isBusy());
    httpSession_->closeWhenIdle();
    cb->writeSuccess();
  }
  EXPECT_FALSE(drainPending_);
  EXPECT_FALSE(reusable_);
}

TEST_F(MockCodecDownstreamTest, send_double_goaway_timeout) {
  testGoaway(true, true);
}
TEST_F(MockCodecDownstreamTest, send_double_goaway_idle) {
  testGoaway(true, false);
}
TEST_F(MockCodecDownstreamTest, send_goaway_timeout) {
  testGoaway(false, true);
}
TEST_F(MockCodecDownstreamTest, send_goaway_idle) {
  testGoaway(false, false);
}

TEST_F(MockCodecDownstreamTest, shutdown_then_error) {
  // Test that we ignore any errors after we shutdown the socket in HTTPSession.
  onIngressImpl([&] (const IOBuf& buf) {
      // This executes as the implementation of HTTPCodec::onIngress()

      InSequence dummy;
      HTTPException err(HTTPException::Direction::INGRESS, "foo");
      err.setHttpStatusCode(400);
      HTTPMessage req = getGetRequest();
      MockHTTPHandler handler;

      // Creates and adds a txn to the session
      codecCallback_->onMessageBegin(1, &req);

      EXPECT_CALL(*codec_, closeOnEgressComplete())
        .WillOnce(Return(false));
      EXPECT_CALL(*codec_, onIngressEOF());
      EXPECT_CALL(mockController_, detachSession(_));

      httpSession_->shutdownTransport();

      codecCallback_->onError(1, err, false);
      return buf.computeChainDataLength();
    });
}