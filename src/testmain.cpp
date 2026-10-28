// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <gtest/gtest.h>
#include <jsi/jsi.h>
#include <thread>
#include "jsi/test/testlib.h"
#include "node-api-jsi/ApiLoaders/V8Api.h"
#include "node-api-jsi/NodeApiJsiRuntime.h"
#include "public/ScriptStore.h"
#include "public/V8JsiRuntime.h"
#include "public/v8_api.h"
#if defined(V8JSI_ENABLE_LIFETIME_TESTS)
#include "public/V8JsiRuntimeTest.h"
#include <jsi/instrumentation.h>
#endif

using namespace Microsoft::NodeApiJsi;

namespace facebook::jsi {

std::vector<facebook::jsi::RuntimeFactory> runtimeGenerators() {
  return std::vector<facebook::jsi::RuntimeFactory>{
#if defined(JSI_V8_IMPL)
      []() -> std::unique_ptr<facebook::jsi::Runtime> {
        v8runtime::V8RuntimeArgs args;
        args.flags.explicitMicrotaskPolicy = true;
        args.flags.enableGCApi = true;
        return v8runtime::makeV8Runtime(std::move(args));
      },
#endif
#if defined(_WIN32) // Node-API not supported on POSIX (LibLoader not implemented)
      []() -> std::unique_ptr<facebook::jsi::Runtime> {
        V8Api *v8Api = V8Api::fromLib();
        V8Api::setCurrent(v8Api);

        jsr_config config{};
        jsr_runtime runtime{};
        napi_env env{};
        v8Api->jsr_create_config(&config);
        v8Api->jsr_config_enable_gc_api(config, true);
        v8Api->jsr_create_runtime(config, &runtime);
        v8Api->jsr_delete_config(config);
        v8Api->jsr_runtime_get_node_api_env(runtime, &env);

        NodeApiEnvScope envScope{env};

        return makeNodeApiJsiRuntime(env, v8Api, [runtime]() { V8Api::current()->jsr_delete_runtime(runtime); });
      }
#endif
  };
};

} // namespace facebook::jsi

using namespace facebook::jsi;

#if defined(V8JSI_ENABLE_LIFETIME_TESTS)
namespace {

v8runtime::HostProxyLifetimeStats LifetimeSnapshot() {
  v8runtime::HostProxyLifetimeStats stats{};
  v8runtime::v8jsi_get_host_proxy_lifetime_stats(&stats);
  return stats;
}

void ExpectAllReclaimed(v8runtime::HostProxyLifetimeStats before) {
  auto after = LifetimeSnapshot();
  EXPECT_EQ(after.proxiesAllocated - before.proxiesAllocated, after.proxiesFreed - before.proxiesFreed);
  EXPECT_EQ(after.trackersAllocated - before.trackersAllocated, after.trackersFreed - before.trackersFreed);
}

struct ReentrantCapture {
  ReentrantCapture(Runtime &runtime, bool &destroyed) : runtime(runtime), destroyed(destroyed) {}
  ~ReentrantCapture() {
    runtime.global().setProperty(runtime, "captureReleased", true);
    runtime.instrumentation().collectGarbage("reentrant capture release");
    destroyed = true;
  }
  Runtime &runtime;
  bool &destroyed;
};

void InstallReentrantFunction(Runtime &runtime, bool &destroyed) {
  auto capture = std::make_shared<ReentrantCapture>(runtime, destroyed);
  auto function = Function::createFromHostFunction(
      runtime,
      PropNameID::forAscii(runtime, "retained"),
      0,
      [capture](Runtime &, const Value &, const Value *, size_t) { return Value::undefined(); });
  runtime.global().setProperty(runtime, "retained", function);
}

} // namespace

TEST(HostProxyLifetime, CollectedCaptureMayReenterRuntime) {
  auto before = LifetimeSnapshot();
  bool destroyed = false;
  std::unique_ptr<Runtime> runtime{v8runtime::v8jsi_create_runtime_for_lifetime_test()};
  InstallReentrantFunction(*runtime, destroyed);
  runtime->instrumentation().collectGarbage("retained capture");
  EXPECT_FALSE(destroyed);
  runtime->global().setProperty(*runtime, "retained", Value::undefined());
  runtime->instrumentation().collectGarbage("unreachable capture");
  EXPECT_TRUE(destroyed);
  EXPECT_TRUE(runtime->global().getProperty(*runtime, "captureReleased").getBool());
  ExpectAllReclaimed(before);
}

TEST(HostProxyLifetime, TeardownCaptureMayReenterRuntime) {
  auto before = LifetimeSnapshot();
  bool destroyed = false;
  std::unique_ptr<Runtime> runtime{v8runtime::v8jsi_create_runtime_for_lifetime_test()};
  InstallReentrantFunction(*runtime, destroyed);
  runtime.reset();
  EXPECT_TRUE(destroyed);
  ExpectAllReclaimed(before);
}

TEST(HostProxyLifetime, TeardownKeepsReleasedFunctionShellsAlive) {
  auto before = LifetimeSnapshot();
  bool calledReleasedFunction = false;
  std::unique_ptr<Runtime> runtime{v8runtime::v8jsi_create_runtime_for_lifetime_test()};
  {
    auto peer = Function::createFromHostFunction(
        *runtime,
        PropNameID::forAscii(*runtime, "peer"),
        0,
        [](Runtime &, const Value &, const Value *, size_t) { return Value(42); });
    runtime->global().setProperty(*runtime, "peer", peer);
    auto capture = std::shared_ptr<int>(new int(0), [rt = runtime.get(), &calledReleasedFunction](int *value) {
      delete value;
      calledReleasedFunction = rt->global().getPropertyAsFunction(*rt, "peer").call(*rt).isUndefined();
    });
    auto function = Function::createFromHostFunction(
        *runtime,
        PropNameID::forAscii(*runtime, "retained"),
        0,
        [capture](Runtime &, const Value &, const Value *, size_t) { return Value::undefined(); });
    runtime->global().setProperty(*runtime, "retained", function);
  }
  runtime.reset();
  EXPECT_TRUE(calledReleasedFunction);
  ExpectAllReclaimed(before);
}

TEST(HostProxyLifetime, HostObjectsAreReclaimed) {
  auto before = LifetimeSnapshot();
  std::unique_ptr<Runtime> runtime{v8runtime::v8jsi_create_runtime_for_lifetime_test()};
  auto host = std::make_shared<HostObject>();
  std::weak_ptr<HostObject> weakHost = host;
  {
    auto object = Object::createFromHostObject(*runtime, host);
    host.reset();
    runtime->instrumentation().collectGarbage("retained host object");
    EXPECT_FALSE(weakHost.expired());
    EXPECT_TRUE(object.isHostObject(*runtime));
  }
  runtime->instrumentation().collectGarbage("unreachable host object");
  EXPECT_TRUE(weakHost.expired());
  ExpectAllReclaimed(before);
}
#endif

TEST(Basic, CreateOneRuntimes) {
  v8runtime::V8RuntimeArgs args;
  args.flags.enableInspector = true;
  args.flags.enableGCApi = true; // It can be enabled only once per process and we need it later for Node-API tests.
  auto runtime = v8runtime::makeV8Runtime(std::move(args));

  runtime->evaluateJavaScript(std::make_unique<facebook::jsi::StringBuffer>("x = 1"), "");
  EXPECT_EQ(runtime->global().getProperty(*runtime, "x").getNumber(), 1);
}

TEST(Basic, CreateManyRuntimes) {
  for (size_t i = 0; i < 100; i++) {
    v8runtime::V8RuntimeArgs args;
    args.flags.enableInspector = true;
    auto runtime = v8runtime::makeV8Runtime(std::move(args));

    runtime->evaluateJavaScript(std::make_unique<facebook::jsi::StringBuffer>("x = 1"), "");
    EXPECT_EQ(runtime->global().getProperty(*runtime, "x").getNumber(), 1);
  }
}

TEST(Basic, MultiThreadIsolate) {
  v8runtime::V8RuntimeArgs args;
  args.flags.enableMultiThread = true;
  auto runtime = v8runtime::makeV8Runtime(std::move(args));

  runtime->evaluateJavaScript(
      std::make_unique<facebook::jsi::StringBuffer>("x = {1:2, '3':4, 5:'six', 'seven':['eight', 'nine']}"), "");

  Runtime &rt = *runtime;

  Object x = rt.global().getPropertyAsObject(rt, "x");
  EXPECT_EQ(x.getProperty(rt, "1").getNumber(), 2);

  std::vector<std::thread> vec_thr;
  for (size_t i = 0; i < 10; ++i) {
    vec_thr.push_back(std::thread([&]() {
      EXPECT_EQ(x.getProperty(rt, PropNameID::forAscii(rt, "1")).getNumber(), 2);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      EXPECT_EQ(x.getProperty(rt, "3").getNumber(), 4);
    }));
  }

  for (size_t i = 0; i < vec_thr.size(); ++i) {
    vec_thr.at(i).join();
  }
}

#ifdef _WIN32 // Node-API not supported on POSIX (LibLoader not implemented)
TEST(Basic, MultiThreadIsolateNodeApi) {
  V8Api *v8Api = V8Api::fromLib();
  V8Api::setCurrent(v8Api);

  jsr_config config{};
  jsr_runtime runtime{};
  napi_env env{};
  v8Api->jsr_create_config(&config);
  v8Api->jsr_config_enable_gc_api(config, true);
  v8Api->v8_config_enable_multithreading(config, true);
  v8Api->jsr_create_runtime(config, &runtime);
  v8Api->jsr_delete_config(config);
  v8Api->jsr_runtime_get_node_api_env(runtime, &env);

  std::unique_ptr<facebook::jsi::Runtime> jsiRuntime;
  {
    NodeApiEnvScope envScope{env};
    jsiRuntime = makeNodeApiJsiRuntime(env, v8Api, [runtime]() { V8Api::current()->jsr_delete_runtime(runtime); });
  }

  jsiRuntime->evaluateJavaScript(
      std::make_unique<facebook::jsi::StringBuffer>("x = {1:2, '3':4, 5:'six', 'seven':['eight', 'nine']}"), "");

  Runtime &rt = *jsiRuntime;

  Object x = rt.global().getPropertyAsObject(rt, "x");
  EXPECT_EQ(x.getProperty(rt, "1").getNumber(), 2);

  std::vector<std::thread> vec_thr;
  for (size_t i = 0; i < 10; ++i) {
    vec_thr.push_back(std::thread([&]() {
      V8Api::setCurrent(v8Api);
      EXPECT_EQ(x.getProperty(rt, PropNameID::forAscii(rt, "1")).getNumber(), 2);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      EXPECT_EQ(x.getProperty(rt, "3").getNumber(), 4);
    }));
  }

  for (size_t i = 0; i < vec_thr.size(); ++i) {
    vec_thr.at(i).join();
  }
}
#endif

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
