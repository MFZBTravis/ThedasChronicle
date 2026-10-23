#pragma once
#include <string>

namespace keep {

struct Reply {
    int status = 200;
    std::string body;
};

// The offline Keep's answer to one request (method, path with any query, body), as the real server would give it:
//   POST /auth                  a signed-in session
//   GET  .../wv/...             the world state from world-state.json (404 if there's no file)
//   PUT/POST .../wv/...         the upload accepted (a minimal world state; the game ignores it, see hooks_worldstate)
//   anything else               200 with an empty JSON object (golden nug files, telemetry, achievements)
// Each request is logged by method and path only.
Reply Answer(const std::string& method, const std::string& path, const std::string& body);

// True once after the world state was served to the game (the player chose to import), then false until the next.
bool TakeWorldStateServed();

} // namespace keep
