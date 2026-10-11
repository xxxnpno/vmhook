#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#include <fstream>
#endif

namespace
{
    class h2_server;
    class connection;
    class statement;
    class prepared_statement;
    class result_set;
    class h2_application : public vmhook::object<h2_application>
    {
    public:
        using object::object;
        static auto server() -> std::unique_ptr<h2_server>;
    };
    class h2_server : public vmhook::object<h2_server>
    {
    public:
        using object::object;
        auto port() const -> std::int32_t { return get_method("getPort")->call(); }
        auto java_class() const -> std::unique_ptr<vmhook::class_object>
        { return get_method("getClass")->call(); }
    };
    class properties : public vmhook::object<properties>
    {
    public:
        using object::object;
        auto set(std::string_view key, std::string_view value) -> void
        { (void)get_method("setProperty")->call(key, value); }
    };
    class h2_driver : public vmhook::object<h2_driver>
    {
    public:
        using object::object;
        static auto load() -> std::unique_ptr<h2_driver>
        { return static_members::get_method("load")->call(); }
        auto connect(std::string_view url, const std::unique_ptr<properties>& options) const
            -> std::unique_ptr<connection>;
    };
    class java_thread : public vmhook::object<java_thread>
    {
    public:
        using object::object;
        static auto current() -> std::unique_ptr<java_thread>
        { return static_members::get_method("currentThread")->call(); }
        auto name() const -> std::string { return get_method("getName")->call().as_string(); }
    };
    class java_system : public vmhook::object<java_system>
    {
    public:
        using object::object;
        static auto collect() -> void { (void)static_members::get_method("gc")->call(); }
    };
    class result_set : public vmhook::object<result_set>
    {
    public:
        using object::object;
        auto next() const -> bool { return get_method("next")->call(); }
        auto get_int(std::int32_t column) const -> std::int32_t
        { return get_method("getInt", "(I)I")->call(column); }
        auto get_long(std::int32_t column) const -> std::int64_t
        { return get_method("getLong", "(I)J")->call(column); }
        auto get_string(std::int32_t column) const -> std::unique_ptr<vmhook::java_string>
        { return get_method("getString", "(I)Ljava/lang/String;")->call(column); }
        auto get_bytes(std::int32_t column) const -> std::unique_ptr<vmhook::array<std::int8_t>>
        { return get_method("getBytes", "(I)[B")->call(column); }
        auto was_null() const -> bool { return get_method("wasNull")->call(); }
        auto close() -> void { (void)get_method("close")->call(); }
    };
    class statement : public vmhook::object<statement>
    {
    public:
        using object::object;
        auto execute(std::string_view sql) const -> bool
        { return get_method("execute", "(Ljava/lang/String;)Z")->call(sql); }
        auto query(std::string_view sql) const -> std::unique_ptr<result_set>
        { return get_method("executeQuery")->call(sql); }
        auto close() -> void { (void)get_method("close")->call(); }
    };
    class prepared_statement : public vmhook::object<prepared_statement>
    {
    public:
        using object::object;
        auto set_int(std::int32_t column, std::int32_t value) -> void
        { (void)get_method("setInt")->call(column, value); }
        auto set_long(std::int32_t column, std::int64_t value) -> void
        { (void)get_method("setLong")->call(column, value); }
        auto set_string(std::int32_t column, const std::unique_ptr<vmhook::java_string>& value) -> void
        { (void)get_method("setString", "(ILjava/lang/String;)V")->call(column, value); }
        auto set_null(std::int32_t column, std::int32_t sql_type) -> void
        { (void)get_method("setNull", "(II)V")->call(column, sql_type); }
        auto set_bytes(std::int32_t column, const std::unique_ptr<vmhook::array<std::int8_t>>& value) -> void
        { (void)get_method("setBytes")->call(column, value); }
        auto update() const -> std::int32_t { return get_method("executeUpdate", "()I")->call(); }
        auto query() const -> std::unique_ptr<result_set>
        { return get_method("executeQuery", "()Ljava/sql/ResultSet;")->call(); }
        auto close() -> void { (void)get_method("close")->call(); }
    };
    class connection : public vmhook::object<connection>
    {
    public:
        using object::object;
        auto create_statement() const -> std::unique_ptr<statement>
        { return get_method("createStatement", "()Ljava/sql/Statement;")->call(); }
        auto prepare(std::string_view sql) const -> std::unique_ptr<prepared_statement>
        { return get_method("prepareStatement", "(Ljava/lang/String;)Ljava/sql/PreparedStatement;")->call(sql); }
        auto auto_commit(bool enabled) -> void { (void)get_method("setAutoCommit")->call(enabled); }
        auto commit() -> void { (void)get_method("commit")->call(); }
        auto rollback() -> void { (void)get_method("rollback")->call(); }
        auto url() const -> std::string
        {
            const std::unique_ptr<vmhook::any_object> metadata{get_method("getMetaData")->call()};
            return metadata->get_method("getURL")->call().as_string();
        }
        auto close() -> void { (void)get_method("close")->call(); }
        auto is_closed() const -> bool { return get_method("isClosed")->call(); }
    };
    auto h2_application::server() -> std::unique_ptr<h2_server>
    { return static_members::get_field("server")->get(); }
    auto h2_driver::connect(std::string_view url, const std::unique_ptr<properties>& options) const
        -> std::unique_ptr<connection>
    { return get_method("connect")->call(url, options); }
    using clock_type = std::chrono::steady_clock;
    auto elapsed_ns(clock_type::time_point began) -> std::int64_t
    { return std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - began).count(); }
    auto require(bool condition, std::string_view message) -> void
    { if (!condition) { throw std::runtime_error{std::format("H2 workload failed: {}", message)}; } }
    auto scalar(const std::unique_ptr<statement>& source, std::string_view sql) -> std::int64_t
    {
        const auto result{source->query(sql)};
        require(result->next(), "scalar query produced no row");
        const auto value{result->get_long(1)};
        require(!result->was_null() && !result->next(), "scalar query cardinality/nullness");
        result->close(); return value;
    }
    auto collection_count(const vmhook::runtime_snapshot& snapshot) -> std::int64_t
    {
        std::int64_t total{};
        for (const auto& collector : snapshot.garbage_collectors)
        { if (collector.collection_count) { total += *collector.collection_count; } }
        return total;
    }
    auto worker(void*) -> DWORD
    {
        char destination[4096]{}, transactions_text[32]{};
        if (!::GetEnvironmentVariableA("VMHOOK_H2_RESULT", destination, sizeof destination)) { return 1; }
        std::ofstream report{destination}; report.setf(std::ios::unitbuf);
        const auto check = [&](std::string_view name, bool passed)
        {
            report << std::format("{}={}\n", name, passed ? "pass" : "fail");
            require(passed, name);
        };
        const auto metric = [&](std::string_view name, const auto& value)
        { report << std::format("{}={}\n", name, value); };
        try
        {
            unsigned transactions{32};
            if (::GetEnvironmentVariableA("VMHOOK_H2_TRANSACTIONS", transactions_text, sizeof transactions_text))
            {
                const std::string_view input{transactions_text};
                const auto parsed{std::from_chars(input.data(), input.data() + input.size(), transactions)};
                require(parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size(), "transaction count");
            }
            require(transactions > 0 && transactions <= 10000, "transaction count bounds");
            const auto os_thread{::GetCurrentThreadId()};
            const auto attached{clock_type::now()};
            vmhook::native_thread_context context{"VMHook-h2-worker"};
            require(context.valid(), context.error());
            metric("context_first_ns", elapsed_ns(attached));
            check("h2_context", vmhook::register_class<h2_application>("H2Application")
                && vmhook::register_class<h2_server>("org/h2/tools/Server")
                && vmhook::register_class<java_system>("java/lang/System")
                && vmhook::register_class<java_thread>("java/lang/Thread"));
            check("h2_native_worker", java_thread::current()->name() == "VMHook-h2-worker"
                && ::GetCurrentThreadId() == os_thread);
            const auto server{h2_application::server()};
            const auto loader{server->java_class()->loader()};
            context.set_context_class_loader(loader);
            const auto load_class = [&](std::string_view binary_name)
            { return vmhook::class_object::for_name(binary_name, loader, true); };
            const auto driver_class{load_class("org.h2.Driver")};
            const auto properties_class{load_class("java.util.Properties")};
            const auto connection_class{load_class("java.sql.Connection")};
            const auto statement_class{load_class("java.sql.Statement")};
            const auto prepared_class{load_class("java.sql.PreparedStatement")};
            const auto result_class{load_class("java.sql.ResultSet")};
            require(driver_class && properties_class && connection_class && statement_class && prepared_class && result_class,
                "ordinary application class resolution");
            require(vmhook::register_class<h2_driver>("org/h2/Driver")
                && vmhook::register_class<properties>("java/util/Properties")
                && vmhook::register_class<connection>("java/sql/Connection")
                && vmhook::register_class<statement>("java/sql/Statement")
                && vmhook::register_class<prepared_statement>("java/sql/PreparedStatement")
                && vmhook::register_class<result_set>("java/sql/ResultSet"), "JDBC wrapper bindings");
            const auto options{vmhook::make_unique<properties>()};
            options->set("user", "sa"); options->set("password", "");
            const auto driver{h2_driver::load()};
            const auto base_url{std::format("jdbc:h2:tcp://127.0.0.1:{}/mem:vmhook_h2_validation", server->port())};
            const auto url{std::format("{};DB_CLOSE_DELAY=-1", base_url)};
            const auto connected{clock_type::now()};
            const auto writer{driver->connect(url, options)};
            const auto reader{driver->connect(url, options)};
            metric("two_connections_first_ns", elapsed_ns(connected));
            require(writer && writer->get_instance() && reader && reader->get_instance(), "TCP JDBC connections");
            const auto write_sql{writer->create_statement()}, read_sql{reader->create_statement()};
            const auto writer_url{writer->url()}, reader_url{reader->url()};
            const auto writer_session{scalar(write_sql, "SELECT SESSION_ID()")};
            const auto reader_session{scalar(read_sql, "SELECT SESSION_ID()")};
            metric("writer_url", writer_url); metric("reader_url", reader_url);
            metric("writer_session", writer_session); metric("reader_session", reader_session);
            check("h2_tcp_application", writer_url == base_url && reader_url == base_url
                && writer_session != reader_session);
            {
                const auto version{read_sql->query("SELECT H2VERSION()")};
                require(version->next(), "H2 version query");
                check("h2_version", version->get_string(1)->to_string() == "2.2.224"); version->close();
            }
            const auto unsupported{driver->connect("jdbc:unsupported:vmhook", options)};
            check("h2_java_null_owner", unsupported && unsupported->get_instance() == nullptr);
            (void)write_sql->execute("CREATE TABLE accounts(id INT PRIMARY KEY, name VARCHAR NOT NULL, balance BIGINT NOT NULL, note VARCHAR, payload VARBINARY)");
            (void)write_sql->execute("CREATE TABLE transfers(id INT PRIMARY KEY, sender INT NOT NULL, recipient INT NOT NULL, amount BIGINT NOT NULL)");
            writer->auto_commit(false);
            const auto insert{writer->prepare("INSERT INTO accounts VALUES(?, ?, ?, ?, ?)")};
            const auto unicode{vmhook::new_string<vmhook::java_string>(std::u16string_view{u"Ren\u00e9e \u6771\u4eac \U0001f600"})};
            const auto ascii{vmhook::new_string<vmhook::java_string>("Bob")};
            const auto java_null{std::make_unique<vmhook::java_string>(nullptr)};
            const std::array<std::int8_t, 5> byte_values{-128, 0, 42, 127, -1};
            const auto bytes{vmhook::make_array<std::int8_t>(static_cast<std::int32_t>(byte_values.size()))};
            bytes->copy_from(byte_values);
            for (std::int32_t id{1}; id <= 2; ++id)
            {
                insert->set_int(1, id); insert->set_string(2, id == 1 ? unicode : ascii);
                insert->set_long(3, id == 1 ? 10000 : 5000);
                if (id == 1) { insert->set_string(4, java_null); } else { insert->set_null(4, 12); }
                insert->set_bytes(5, bytes); require(insert->update() == 1, "insert row count");
            }
            writer->commit();
            check("h2_prepared_inserts", scalar(read_sql, "SELECT COUNT(*) FROM accounts") == 2
                && scalar(read_sql, "SELECT SUM(balance) FROM accounts") == 15000);
            const auto observed_before{vmhook::observe_runtime()};
            {
                const auto rows{read_sql->query("SELECT id, name, note, payload FROM accounts ORDER BY id")};
                java_system::collect();
                require(rows->next() && rows->get_int(1) == 1, "first account after GC");
                const auto name{rows->get_string(2)};
                require(!rows->was_null(), "Unicode value marked SQL null");
                const auto note{rows->get_string(3)};
                require(note && !note->get_instance() && rows->was_null(), "SQL null versus C++ owner");
                const auto payload{rows->get_bytes(4)};
                java_system::collect();
                check("h2_unicode_null_arrays", name->to_utf16() == u"Ren\u00e9e \u6771\u4eac \U0001f600"
                    && payload->to_vector() == std::vector<std::int8_t>(byte_values.begin(), byte_values.end()));
                require(rows->next() && rows->get_int(1) == 2, "second account");
                require(rows->get_string(2)->to_string() == "Bob" && !rows->was_null(), "second account name");
                const auto other_null{rows->get_string(3)};
                require(other_null && !other_null->get_instance() && rows->was_null() && !rows->next(), "second SQL null/cardinality");
                rows->close();
            }
            const auto debit{writer->prepare("UPDATE accounts SET balance=balance-? WHERE id=?")};
            const auto credit{writer->prepare("UPDATE accounts SET balance=balance+? WHERE id=?")};
            const auto audit{writer->prepare("INSERT INTO transfers VALUES(?, ?, ?, ?)")};
            const auto balance{reader->prepare("SELECT balance FROM accounts WHERE id=?")};
            const auto read_balance = [&](std::int32_t id)
            {
                balance->set_int(1, id); const auto row{balance->query()};
                require(row->next(), "balance query row"); const auto value{row->get_long(1)};
                require(!row->was_null() && !row->next(), "balance query cardinality/nullness"); row->close(); return value;
            };
            credit->set_long(1, 777); credit->set_int(2, 1); require(credit->update() == 1, "rollback update count");
            require(scalar(write_sql, "SELECT balance FROM accounts WHERE id=1") == 10777 && read_balance(1) == 10000,
                "uncommitted transaction visibility");
            writer->rollback();
            check("h2_rollback_isolation", read_balance(1) == 10000 && read_balance(2) == 5000);
            bool duplicate_error{}, syntax_error{};
            try { (void)insert->update(); }
            catch (const vmhook::exception& error)
            {
                const auto throwable{error.get_throwable()}; java_system::collect();
                duplicate_error = error.code() == vmhook::error_code::java_exception
                    && throwable->get_method("getSQLState")->call().as_string() == "23505"
                    && static_cast<std::int32_t>(throwable->get_method("getErrorCode")->call()) == 23505;
            }
            writer->rollback();
            try { (void)write_sql->execute("SELEKT balance FROM accounts"); }
            catch (const vmhook::exception& error)
            {
                const auto throwable{error.get_throwable()}; java_system::collect();
                syntax_error = error.code() == vmhook::error_code::java_exception
                    && throwable->get_method("getSQLState")->call().as_string() == "42001"
                    && static_cast<std::int32_t>(throwable->get_method("getErrorCode")->call()) == 42001;
            }
            writer->rollback();
            check("h2_sql_exceptions", duplicate_error && syntax_error && read_balance(1) == 10000);
            std::vector<std::int64_t> samples{}; samples.reserve(transactions);
            std::int64_t moved{};
            const auto workload_start{clock_type::now()};
            for (unsigned index{}; index < transactions; ++index)
            {
                const std::int64_t amount{static_cast<std::int64_t>(index % 7 + 1)};
                const auto transaction_start{clock_type::now()};
                debit->set_long(1, amount); debit->set_int(2, 1); require(debit->update() == 1, "debit row count");
                credit->set_long(1, amount); credit->set_int(2, 2); require(credit->update() == 1, "credit row count");
                audit->set_int(1, static_cast<std::int32_t>(index)); audit->set_int(2, 1);
                audit->set_int(3, 2); audit->set_long(4, amount); require(audit->update() == 1, "audit row count");
                writer->commit(); moved += amount;
                require(read_balance(1) == 10000 - moved && read_balance(2) == 5000 + moved,
                    "committed transaction visibility");
                samples.push_back(elapsed_ns(transaction_start));
                if (index % 8 == 0) { java_system::collect(); }
            }
            const auto workload_ns{elapsed_ns(workload_start)};
            check("h2_committed_transfers", scalar(read_sql, "SELECT COUNT(*) FROM transfers") == transactions
                && scalar(read_sql, "SELECT SUM(amount) FROM transfers") == moved
                && scalar(read_sql, "SELECT SUM(balance) FROM accounts") == 15000);
            const auto observed_after{vmhook::observe_runtime()};
            check("h2_live_gc_roots", collection_count(observed_after) > collection_count(observed_before)
                && unicode->to_utf16() == u"Ren\u00e9e \u6771\u4eac \U0001f600" && ::GetCurrentThreadId() == os_thread);
            metric("transactions", transactions); metric("transferred_amount", moved);
            metric("workload_wall_ns", workload_ns);
            for (std::size_t index{}; index < samples.size(); ++index)
            { metric(std::format("transaction_sample_{}_ns", index), samples[index]); }
            std::sort(samples.begin(), samples.end());
            metric("transaction_p50_ns", samples[(samples.size() * 50 + 99) / 100 - 1]);
            metric("transaction_p99_ns", samples[(samples.size() * 99 + 99) / 100 - 1]);
            metric("transaction_min_ns", samples.front()); metric("transaction_max_ns", samples.back());
            metric("gc_collection_delta", collection_count(observed_after) - collection_count(observed_before));
            metric("heap_used_before_bytes", observed_before.heap.used_bytes);
            metric("heap_used_after_bytes", observed_after.heap.used_bytes);
            insert->close(); debit->close(); credit->close(); audit->close(); balance->close();
            (void)write_sql->execute("DROP ALL OBJECTS");
            write_sql->close(); read_sql->close(); writer->close(); reader->close();
            check("h2_closed_resources", writer->is_closed() && reader->is_closed());
            check("h2_complete", true); return 0;
        }
        catch (const std::exception& error) { report << std::format("exception={}\n", error.what()); return 2; }
    }
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(instance);
        HANDLE thread = ::CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) { ::CloseHandle(thread); }
    }
    return true;
}
