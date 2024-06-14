#include <string>
#include <iostream>
#include <photon/photon.h>
#include <photon/integration/redis.h>
#include <photon/net/iostream.h>
#include <cpp_redis/cpp_redis>
#include <cpp_redis/misc/macro.hpp>
#include <redis-cpp/execute.h>

// this function is copied from the official site of cpp_redis
void cpp_redis_official(cpp_redis::client& client) {
	// cpp_redis::client client;
	client.connect("127.0.0.1", 6379,
        [](const std::string &host, std::size_t port, cpp_redis::connect_state status) {
                if (status == cpp_redis::connect_state::dropped) {
                    std::cout << "client disconnected from " << host << ":" << port << std::endl;
                }
        });

	auto replcmd = [](const cpp_redis::reply &reply) {
			std::cout << "set hello 42: " << reply << std::endl;
			// if (reply.is_string())
			//   do_something_with_string(reply.as_string())
	};
	const std::string group_name = "groupone";
	const std::string session_name = "sessone";
	const std::string consumer_name = "ABCD";

	std::multimap<std::string, std::string> ins;
	ins.insert(std::pair<std::string, std::string>{"message", "hello"});

#ifdef ENABLE_SESSION

	client.xadd(session_name, "*", ins, replcmd);
	client.xgroup_create(session_name, group_name, "0", replcmd);

	client.sync_commit();

	client.xrange(session_name, {"-", "+", 10}, replcmd);

	client.xreadgroup({group_name,
	                   consumer_name,
	                   {{session_name}, {">"}},
	                   1, // Count
	                   0, // block milli
	                   false, // no ack
	                  }, [](cpp_redis::reply &reply) {
			std::cout << "set hello 42: " << reply << std::endl;
			auto msg = reply.as_array();
			std::cout << "Mes: " << msg[0] << std::endl;
			// if (reply.is_string())
			//   do_something_with_string(reply.as_string())
	});

#else

	// same as client.send({ "SET", "hello", "42" }, ...)
	client.set("hello", "42", [](cpp_redis::reply &reply) {
			std::cout << "set hello 42: " << reply << std::endl;
			// if (reply.is_string())
			//   do_something_with_string(reply.as_string())
	});

	// same as client.send({ "DECRBY", "hello", 12 }, ...)
	client.decrby("hello", 12, [](cpp_redis::reply &reply) {
			std::cout << "decrby hello 12: " << reply << std::endl;
			// if (reply.is_integer())
			//   do_something_with_integer(reply.as_integer())
	});

	// same as client.send({ "GET", "hello" }, ...)
	client.get("hello", [](cpp_redis::reply &reply) {
			std::cout << "get hello: " << reply << std::endl;
			// if (reply.is_string())
			//   do_something_with_string(reply.as_string())
	});

#endif

	// commands are pipelined and only sent when client.commit() is called
	// client.commit();

	// synchronous commit, no timeout
	client.sync_commit();

	// synchronous commit, timeout
	// client.sync_commit(std::chrono::milliseconds(100));
}

// this function is copied from the official site of redis_cpp
int redis_cpp_official(std::iostream* stream) {
    try
    {
        // auto stream = rediscpp::make_stream("localhost", "6379");

        auto const key = "my_key";

        auto response = rediscpp::execute(*stream, "set",
                key, "Some value for 'my_key'", "ex", "60");

        std::cout << "Set key '" << key << "': " << response.as<std::string>() << std::endl;

        response = rediscpp::execute(*stream, "get", key);
        std::cout << "Get key '" << key << "': " << response.as<std::string>() << std::endl;
    }
    catch (std::exception const &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

// this function demostrates how to integrate photon with cpp_redis
void demo_cpp_redis() {
    auto tcp_client = photon::integration::new_tcp_client_redis();
    cpp_redis::client client(tcp_client);
    cpp_redis(client);
    delete tcp_client;
}

// this function demostrates how to integrate photon with redis_cpp
void demo_redis_cpp() {
	auto tcp_client = photon::net::new_tcp_socket_client();
	auto s = tcp_client->connect({"127.0.0.1", 6379});
	auto ios = photon::net::new_iostream(s);
	redis_cpp_official(ios);
	delete ios;
	delete s;
	delete tcp_client;
}

int main() {
	photon::init();
	demo_redis_cpp();
	demo_cpp_redis();
	photon::fini();
	return 0;
}
