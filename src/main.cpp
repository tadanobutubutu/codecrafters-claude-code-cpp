#include <cstdlib>
#include <iostream>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <memory>
#include <array>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

// Read and return the contents of a file
std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return "Error: Could not open file " + path;
    }
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

// Write content to a file, creating directories if needed
std::string write_file(const std::string& path, const std::string& content) {
    try {
        fs::path filepath(path);
        if (filepath.has_parent_path()) {
            fs::create_directories(filepath.parent_path());
        }
        std::ofstream ofs(path);
        if (!ofs.is_open()) {
            return "Error: Could not open file " + path + " for writing";
        }
        ofs << content;
        return "Successfully wrote to " + path;
    } catch (const std::exception& e) {
        return "Error: " + std::string(e.what());
    }
}

// Execute a shell command and return output
std::string exec_bash(const std::string& cmd) {
    std::string command = cmd + " 2>&1";
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        return "Error: popen() failed.";
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result.empty() ? "Command executed successfully (no output)" : result;
}

int main(int argc, char* argv[]) {
    if (argc < 3 || std::string(argv[1]) != "-p") {
        std::cerr << "Expected first argument to be '-p'" << std::endl;
        return 1;
    }

    std::string prompt = argv[2];

    if (prompt.empty()) {
        std::cerr << "Prompt must not be empty" << std::endl;
        return 1;
    }

    const char* api_key_env = std::getenv("OPENROUTER_API_KEY");
    const char* base_url_env = std::getenv("OPENROUTER_BASE_URL");

    std::string api_key = api_key_env ? api_key_env : "";
    std::string base_url = base_url_env ? base_url_env : "https://openrouter.ai/api/v1";

    if (api_key.empty()) {
        std::cerr << "OPENROUTER_API_KEY is not set" << std::endl;
        return 1;
    }

    // Tool definitions
    json tools = json::array({
        {
            {"type", "function"},
            {"function", {
                {"name", "Read"},
                {"description", "Read and return the contents of a file"},
                {"parameters", {
                    {"type", "object"},
                    {"required", json::array({"file_path"})},
                    {"properties", {
                        {"file_path", {
                            {"type", "string"},
                            {"description", "The path to the file to read"}
                        }}
                    }}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "Write"},
                {"description", "Write content to a file"},
                {"parameters", {
                    {"type", "object"},
                    {"required", json::array({"file_path", "content"})},
                    {"properties", {
                        {"file_path", {
                            {"type", "string"},
                            {"description", "The path of the file to write to"}
                        }},
                        {"content", {
                            {"type", "string"},
                            {"description", "The content to write to the file"}
                        }}
                    }}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "Bash"},
                {"description", "Execute a shell command"},
                {"parameters", {
                    {"type", "object"},
                    {"required", json::array({"command"})},
                    {"properties", {
                        {"command", {
                            {"type", "string"},
                            {"description", "The command to execute"}
                        }}
                    }}
                }}
            }}
        }
    });

    // Initialize messages list with user prompt
    json messages = json::array({
        {{"role", "user"}, {"content", prompt}}
    });

    // Agent loop
    while (true) {
        json request_body = {
            {"model", "anthropic/claude-haiku-4.5"},
            {"messages", messages},
            {"tools", tools}
        };

        cpr::Response response = cpr::Post(
            cpr::Url{base_url + "/chat/completions"},
            cpr::Header{
                {"Authorization", "Bearer " + api_key},
                {"Content-Type", "application/json"}
            },
            cpr::Body{request_body.dump()}
        );

        if (response.status_code != 200) {
            std::cerr << "HTTP error: " << response.status_code << std::endl;
            std::cerr << "Response text: " << response.text << std::endl;
            return 1;
        }

        json result = json::parse(response.text);

        if (!result.contains("choices") || result["choices"].empty()) {
            std::cerr << "No choices in response" << std::endl;
            return 1;
        }

        json assistant_message = result["choices"][0]["message"];
        
        // Append assistant message to conversation
        messages.push_back(assistant_message);

        // Check if there are tool calls
        if (assistant_message.contains("tool_calls") && !assistant_message["tool_calls"].empty()) {
            for (const auto& tool_call : assistant_message["tool_calls"]) {
                std::string name = tool_call["function"]["name"];
                std::string raw_args = tool_call["function"]["arguments"];
                json args = json::parse(raw_args);
                std::string tool_result;

                if (name == "Read") {
                    tool_result = read_file(args["file_path"].get<std::string>());
                } else if (name == "Write") {
                    tool_result = write_file(args["file_path"].get<std::string>(), args["content"].get<std::string>());
                } else if (name == "Bash") {
                    tool_result = exec_bash(args["command"].get<std::string>());
                } else {
                    tool_result = "Unknown tool: " + name;
                }

                // Append tool result
                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", tool_call["id"]},
                    {"content", tool_result}
                });
            }
            // Send the tool results back in the next loop iteration
            continue;
        }

        // Print final content to stdout
        std::cerr << "Logs from your program will appear here!" << std::endl;
        std::cout << assistant_message["content"].get<std::string>();
        break;
    }

    return 0;
}
