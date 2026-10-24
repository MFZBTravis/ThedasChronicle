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
//   golden nug file             with Mode=earned: uploads kept in ThedasChronicleNug.json, reads answered from it
//   anything else               200 with an empty JSON object (golden nug with Mode=all, telemetry, achievements)
// Each request is logged by method and path only.
Reply Answer(const std::string& method, const std::string& path, const std::string& body);

// What happened to the game's last world-state request, reported once and then reset to None:
//   Served   the player chose to import and world-state.json was sent
//   NoFile   the player chose to import but there's no readable world-state.json
//   None     no request reached the offline Keep (the import was declined, or the game isn't signed in to the Keep)
enum class WorldStateRequest { None, Served, NoFile };
WorldStateRequest TakeWorldStateRequest();

} // namespace keep
