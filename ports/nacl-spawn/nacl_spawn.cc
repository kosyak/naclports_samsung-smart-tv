// Copyright (c) 2014 The Native Client Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Emulates spawning/waiting process by asking JavaScript to do so.

#include <assert.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/var_array.h"
#include "ppapi/cpp/var_dictionary.h"
#include "ppapi_simple/ps_instance.h"

#if defined(__GLIBC__)
# include "library_dependencies.h"
#endif
#include "path_util.h"

extern char** environ;

struct NaClSpawnReply {
  pthread_mutex_t mu;
  pthread_cond_t cond;
  // Zero or positive on success or -errno on failure.
  int result;
};

static std::string GetCwd() {
  char cwd[PATH_MAX + 1];
  // TODO(hamaji): Remove this #if and always call getcwd.
  // https://code.google.com/p/naclports/issues/detail?id=109
#if defined(__GLIBC__)
  if (!getwd(cwd))
#else
  if (!getcwd(cwd, PATH_MAX))
#endif
    assert(0);
  return cwd;
}

static std::string GetAbsPath(const std::string& path) {
  assert(!path.empty());
  if (path[0] == '/')
    return path;
  else
    return GetCwd() + '/' + path;
}

// Returns a unique request ID to make all request strings different
// from each other.
static std::string GetRequestId() {
  static int64_t req_id = 0;
  static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&mu);
  int64_t id = ++req_id;
  pthread_mutex_unlock(&mu);
  char buf[64];
  sprintf(buf, "%lld", id);
  return buf;
}

// Handle reply from JavaScript. The key is the request string and the
// value is Zero or positive on success or -errno on failure. The
// user_data must be an instance of NaClSpawnReply.
static void HandleNaClSpawnReply(const pp::Var& key,
                                 const pp::Var& value,
                                 void* user_data) {
  if (!key.is_string() || !value.is_int()) {
    fprintf(stderr, "Invalid parameter for HandleNaClSpawnReply\n");
    fprintf(stderr, "key=%s\n", key.DebugString().c_str());
    fprintf(stderr, "value=%s\n", value.DebugString().c_str());
  }
  assert(key.is_string());
  assert(value.is_int());

  NaClSpawnReply* reply = static_cast<NaClSpawnReply*>(user_data);
  pthread_mutex_lock(&reply->mu);
  reply->result = value.AsInt();
  pthread_cond_signal(&reply->cond);
  pthread_mutex_unlock(&reply->mu);
}

// Sends a spawn/wait request to JavaScript and returns the result.
static int SendRequest(pp::VarDictionary* req) {
  const std::string& req_id = GetRequestId();
  req->Set("id", req_id);

  NaClSpawnReply reply;
  pthread_mutex_init(&reply.mu, NULL);
  pthread_cond_init(&reply.cond, NULL);
  PSInstance* instance = PSInstance::GetInstance();
  instance->RegisterMessageHandler(req_id, &HandleNaClSpawnReply, &reply);

  instance->PostMessage(*req);

  pthread_mutex_lock(&reply.mu);
  pthread_cond_wait(&reply.cond, &reply.mu);
  pthread_mutex_unlock(&reply.mu);

  pthread_cond_destroy(&reply.cond);
  pthread_mutex_destroy(&reply.mu);

  instance->RegisterMessageHandler(req_id, NULL, &reply);
  return reply.result;
}

// Adds a file into nmf. |key| is the key for open_resource IRT or
// "program". |filepath| is not a URL yet. JavaScript code is
// responsible to fix them.
static void AddFileToNmf(const std::string& key,
                         const std::string& filepath,
                         pp::VarDictionary* dict) {
#if defined(__x86_64__)
  static const char kArch[] = "x86-64";
#elif defined(__i686__)
  static const char kArch[] = "x86-32";
#elif defined(__arm__)
  static const char kArch[] = "arm";
#elif defined(__pnacl__)
  static const char kArch[] = "pnacl";
#else
# error "Unknown architecture"
#endif
  pp::VarDictionary url;
  url.Set("url", filepath);
  pp::VarDictionary arch;
  arch.Set(kArch, url);
  dict->Set(key, arch);
}

static void AddNmfToRequestForShared(
  std::string prog,
  const std::vector<std::string>& dependencies,
  pp::VarDictionary* req) {
  pp::VarDictionary nmf;
  pp::VarDictionary files;
  const char* prog_base = basename(&prog[0]);
  for (size_t i = 0; i < dependencies.size(); i++) {
    std::string dep = dependencies[i];
    const std::string& abspath = GetAbsPath(dep);
    const char* base = basename(&dep[0]);
    // nacl_helper does not pass the name of program and the dynamic
    // loader always uses "main.nexe" as the main binary.
    if (!strcmp(prog_base, base))
      base = "main.nexe";
    AddFileToNmf(base, abspath, &files);
  }
  nmf.Set("files", files);

#if defined(__x86_64__)
  static const char kDynamicLoader[] = "lib64/runnable-ld.so";
#else
  static const char kDynamicLoader[] = "lib32/runnable-ld.so";
#endif
  AddFileToNmf("program", kDynamicLoader, &nmf);

  req->Set("nmf", nmf);
}

static void AddNmfToRequestForStatic(const std::string& prog,
                                     pp::VarDictionary* req) {
  pp::VarDictionary nmf;
  AddFileToNmf("program", GetAbsPath(prog), &nmf);
  req->Set("nmf", nmf);
}

// Adds a NMF to the request if |prog| is stored in HTML5 filesystem.
static bool AddNmfToRequest(std::string prog, pp::VarDictionary* req) {
  if (prog.find('/') == std::string::npos) {
    bool found = false;
    const char* path_env = getenv("PATH");
    std::vector<std::string> paths;
    GetPaths(path_env, &paths);
    if (paths.empty() || !GetFileInPaths(prog, paths, &prog)) {
      // If the path does not contain a slash and we cannot find it
      // from PATH, we use NMF served with the JavaScript.
      return true;
    }
  }

  if (access(prog.c_str(), R_OK) != 0) {
    errno = ENOENT;
    return false;
  }

#if defined(__GLIBC__)
  std::vector<std::string> dependencies;
  if (!FindLibraryDependencies(prog, &dependencies))
    return false;

  if (!dependencies.empty()) {
    AddNmfToRequestForShared(prog, dependencies, req);
    return true;
  }
  // No dependencies means the main binary is statically linked.
#endif
  AddNmfToRequestForStatic(prog, req);
  return true;
}

// Spawn a new NaCl process by asking JavaScript. This function lacks
// a lot of features posix_spawn supports (e.g., handling FDs).
// Returns 0 on success. On error -1 is returned and errno will be set
// appropriately.
extern "C" int nacl_spawn_simple(const char** argv) {
  assert(argv[0]);
  pp::VarDictionary req;
  req.Set("command", "nacl_spawn");
  pp::VarArray args;
  for (int i = 0; argv[i]; i++)
    args.Set(i, argv[i]);
  req.Set("args", args);
  pp::VarArray envs;
  for (int i = 0; environ[i]; i++)
    envs.Set(i, environ[i]);
  req.Set("envs", envs);
  req.Set("cwd", GetCwd());

  if (!AddNmfToRequest(argv[0], &req)) {
    errno = ENOENT;
    return -1;
  }

  int result = SendRequest(&req);
  if (result < 0) {
    errno = -result;
    return -1;
  }
  return result;
}

// Waits for the specified pid. The semantics of this function is as
// same as waitpid, though this implementation has some restrictions.
// Returns 0 on success. On error -1 is returned and errno will be set
// appropriately.
extern "C" int nacl_waitpid(int pid, int* status, int options) {
  // We only support waiting for a single process.
  assert(pid > 0);
  // No options are supported yet.
  assert(options == 0);

  pp::VarDictionary req;
  req.Set("command", "nacl_wait");
  req.Set("pid", pid);

  int result = SendRequest(&req);
  if (result < 0) {
    errno = -result;
    return -1;
  }

  // WEXITSTATUS(s) is defined as ((s >> 8) & 0xff).
  if (status)
    *status = (result & 0xff) << 8;
  return 0;
}
