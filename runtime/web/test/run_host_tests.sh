#!/bin/sh
# run_host_tests.sh -- build and run the web service's host tests on the M4
# with the host's C++ compiler (${CXX:-c++}, version printed), under address
# and undefined-behaviour sanitizers with no recovery: a finding fails the
# run. Only the pure files take part (json, http, auth, config_edit, api,
# api_pages, with the supervisor's configuration, protocol and server
# behind the API test's fake backend); tls_server.cpp, system_backend.cpp,
# crypto_system.cpp, control_client.cpp and stxweb.cpp are FreeBSD's and
# OpenSSL's and build in build_programs.sh (the VM scenario
# appliance/web/vm/c2_web.sh runs them). Output in
# <storage>/appliance/host_tests/. Exit status 0 only when every suite
# passes.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
web=$(cd "$here/.." && pwd)
supervisor=$(cd "$web/../supervisor" && pwd)
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
out=$storage/appliance/host_tests
compiler=${CXX:-c++}
mkdir -p "$out"
"$compiler" --version | head -1
flags="-std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
       -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror"
pure="$web/json.cpp $web/http.cpp $web/auth.cpp $web/config_edit.cpp $web/api.cpp $web/api_pages.cpp
      $supervisor/config/boot_config.cpp $supervisor/config/user_config.cpp $supervisor/control/control_protocol.cpp"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_web" "$here/test_web.cpp" $pure
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_api" "$here/test_api.cpp" $pure "$supervisor/control/control_server.cpp" \
    "$supervisor/config/service_registry.cpp" "$supervisor/boot_slots.cpp"
status=0
"$out/test_web" || status=1
"$out/test_api" || status=1
echo "run_host_tests: status=$status"
exit $status
