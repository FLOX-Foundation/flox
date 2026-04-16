# gen_js_inc.cmake — convert a JS file into a C string literal .inc file
# Usage: cmake -DIN_FILE=foo.js -DOUT_FILE=foo.inc -P gen_js_inc.cmake

file(READ "${IN_FILE}" CONTENT)
string(REPLACE "\\" "\\\\" CONTENT "${CONTENT}")
string(REPLACE "\"" "\\\"" CONTENT "${CONTENT}")
string(REPLACE "\n" "\\n\"\n\"" CONTENT "${CONTENT}")
file(WRITE "${OUT_FILE}" "\"${CONTENT}\"")
