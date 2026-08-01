#include "proxy/upstream.hpp"

#include "channels/channels.hpp"
#include "config/config.hpp"
#include "store/database.hpp"
#include "store/mysql_test_env.hpp"
#include "store/schema.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

int expect(bool ok, const char *message)
{
    if (ok) {
        return 0;
    }
    std::cerr << message << '\n';
    return 1;
}

std::string header_value(const std::vector<revlm::UpstreamHeader> &headers, std::string_view name)
{
    for (const auto &header : headers) {
        std::string left = header.name;
        std::string right = std::string{ name };
        for (char &ch : left) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        for (char &ch : right) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        if (left == right) {
            return header.value;
        }
    }
    return {};
}

long long seed_channel(std::string_view type, std::string_view name, std::string_view base_url,
                       std::string_view api_key)
{
    revlm::Channel channel(0, std::string{ type }, std::string{ name }, true, 0, std::string{ base_url },
                           std::string{ api_key }, 1.0);
    if (!revlm::ChannelStore::instance().create_channel(channel)) {
        throw std::runtime_error("create_channel failed");
    }
    return channel.id;
}

} // namespace

int main()
{
    std::optional<revlm::test::MysqlTestEnv> env = revlm::test::prepare_mysql_test_env("upstream");
    if (!env.has_value()) {
        return 0;
    }

    try {
        auto db = revlm::make_database(env->dsn);
        revlm::ensure_schema(*db);
        revlm::Config config;
        config.db_dsn = env->dsn;
        revlm::test::install_test_runtime(config);
        revlm::sql_exec(*db, "DELETE FROM channels");
    } catch (const std::exception &ex) {
        std::cerr << "mysql setup failed: " << ex.what() << '\n';
        return 1;
    }

    const long long openai_id = seed_channel("openai_compatible", "openai", "https://api.example.test/v1", "sk-openai");
    const long long anthropic_id =
        seed_channel("anthropic", "anthropic", "https://claude.example.test", "sk-anthropic");
    const long long blocked_id = seed_channel("openai_compatible", "blocked", "http://127.0.0.1:18080", "sk-blocked");

    revlm::UpstreamExecutor executor;

    {
        revlm::UpstreamRequest request;
        request.method = "POST";
        request.path = "/v1/responses";
        request.body = R"({"model":"gpt-5"})";
        bool rejected = false;
        try {
            (void)executor.prepare(blocked_id, request);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        if (expect(rejected, "executor should reject blocked SSRF upstream targets") != 0) {
            return 1;
        }
    }

    {
        revlm::UpstreamRequest request{
            .method = "POST",
            .path = "/v1/responses",
            .query = "stream=true",
            .headers = { { "Authorization", "Bearer user" }, { "Accept-Encoding", "gzip" }, { "X-Test", "ok" } },
            .body = R"({"model":"gpt-5","max_output_tokens":32,"stream":true})",
        };
        const auto prepared = executor.prepare(openai_id, request);
        if (expect(prepared.url == "https://api.example.test/v1/responses?stream=true",
                   "openai base /v1 should collapse downstream /v1 prefix") != 0 ||
            expect(header_value(prepared.headers, "authorization") == "Bearer sk-openai",
                   "openai executor should inject upstream bearer token") != 0 ||
            expect(header_value(prepared.headers, "accept-encoding") == "identity",
                   "openai executor should force identity encoding") != 0 ||
            expect(header_value(prepared.headers, "x-test") == "ok", "openai executor should preserve safe headers") !=
                0) {
            return 1;
        }
    }

    {
        revlm::UpstreamRequest request{
            .method = "POST",
            .path = "/v1/messages",
            .query = {},
            .headers = {},
            .body = R"({"model":"claude","stream":false})",
        };
        const auto prepared = executor.prepare(anthropic_id, request);
        if (expect(prepared.url == "https://claude.example.test/v1/messages",
                   "anthropic executor should keep message path") != 0 ||
            expect(header_value(prepared.headers, "x-api-key") == "sk-anthropic",
                   "anthropic executor should inject x-api-key") != 0 ||
            expect(header_value(prepared.headers, "anthropic-version") == "2023-06-01",
                   "anthropic executor should default version header") != 0) {
            return 1;
        }
    }

    return 0;
}
