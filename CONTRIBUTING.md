# Contributing

1. Fork and create a topic branch.
2. Install ESP-IDF v5.4.1 and make sure `idf.py build` passes for `esp32c6`.
3. Match the surrounding C style (4 spaces, `.clang-format` provided); keep functions small
   and avoid large stack buffers — task stacks are tight.
4. Describe what you tested (hardware, hub, broker) in the pull request.

For web UI work you can run `python tools/mock_server.py` (needs `flask`) instead of flashing.
