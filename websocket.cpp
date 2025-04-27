#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/evp.h>
#include <json/json.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <chrono>
#include <ctime>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>
#include <iomanip>
#include <format>
#include <sstream>
#include <thread>
using namespace std;

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// Генератор подписи для аутентификации
std::string generate_signature(const std::string& secret, const std::string& message) {

    unsigned char digest[EVP_MAX_MD_SIZE];
    size_t len = 0; // Изменено на size_t

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    const EVP_MD* md = EVP_sha256();
    EVP_PKEY* pkey = EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, nullptr,
        (const unsigned char*)secret.c_str(),
        secret.length());

    EVP_DigestSignInit(mdctx, nullptr, md, nullptr, pkey);
    EVP_DigestSignUpdate(mdctx, message.c_str(), message.length());
    EVP_DigestSignFinal(mdctx, digest, &len); // Теперь &len корректного типа

    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);

    std::stringstream ss;
    for (size_t i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return ss.str();
}

// Запрос аутентификации
std::string create_auth_request(const std::string& api_key, const std::string& secret_key) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto expires = std::chrono::duration_cast<std::chrono::milliseconds>(now).count() + 5000;

    std::string message = "GET/realtime" + std::to_string(expires);
    std::string signature = generate_signature(secret_key, message);

    return R"({"op":"auth","args":[")" + api_key + R"(",")" + std::to_string(expires) + R"(",")" + signature + R"("]})";
}

// Запрос подписки на каналы
std::string create_subscribe_request() {
    return R"({
  "op": "subscribe",
  "args": [
    "publicTrade.BTCUSDT"
  ]
})";
}

std::string timestamp_to_time(uint64_t millis_timestamp) {

    using namespace std::chrono;
     
    // 1. Преобразуем миллисекунды в системное время
    sys_time<milliseconds> tp{ milliseconds(millis_timestamp)}; 

    // 2. Получаем продолжительность с начала текущих суток
    auto dp = floor<days>(tp);  // Целое количество дней
    auto time_of_day = tp - dp;  // Время с начала суток

    // 3. Разбиваем на компоненты
    auto h = duration_cast<hours>(time_of_day);
    time_of_day -= h;
    auto m = duration_cast<minutes>(time_of_day);
    time_of_day -= m;
    auto s = duration_cast<seconds>(time_of_day);
    time_of_day -= s;
    auto ms = duration_cast<milliseconds>(time_of_day);

    // 4. Форматируем вывод
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(2) << h.count() << ":"
        << std::setw(2) << m.count() << ":"
        << std::setw(2) << s.count() << "."
        << std::setw(3) << ms.count();

    return oss.str();
   
}

int main()
{
    try {
    
        const std::string api_key = "your api_key";
        const std::string secret_key = "your secret_key";

        // Контекст ввода-вывода и SSL
        net::io_context ioc;
        ssl::context ctx{ ssl::context::tlsv12_client };
        ctx.set_verify_mode(ssl::verify_none);

        // Разрешение имени хоста
        tcp::resolver resolver{ ioc };
        auto const results = resolver.resolve("stream.bybit.com", "443");

        // Создание WebSocket соединения
        websocket::stream<beast::ssl_stream<tcp::socket>> ws{ ioc, ctx };

        // Установка SNI Hostname
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), "stream.bybit.com")) {
            beast::error_code ec{ static_cast<int>(::ERR_get_error()), net::error::get_ssl_category() };
            throw beast::system_error{ ec };
        }

         // Подключение
        net::connect(ws.next_layer().next_layer(), results.begin(), results.end());

       ws.next_layer().handshake(ssl::stream_base::client);

       ws.handshake("stream.bybit.com", "/v5/public/spot");

       std::cout << "Connected to Bybit WebSocket API\n";

        // Аутентификация
        std::string auth_request = create_auth_request(api_key, secret_key);
        ws.write(net::buffer(auth_request));
        std::cout << "Authentication request sent\n";

        std::string subscribe_request = create_subscribe_request();
        ws.write(net::buffer(subscribe_request));
        std::cout << "Subscription request sent\n";

        // Буфер для сообщений
        beast::flat_buffer buffer;
        std::string message = "";
        thread async_read([&]()  //асинхронное получение данных
            {
                while (1)
                {
                    ws.read(buffer);
                    message = beast::buffers_to_string(buffer.data());
                    buffer.consume(buffer.size());
                }
        });

       // Основной цикл получения данных
        while (true) {

           //  парсинг JSON
            while (message.empty());
            Json::CharReaderBuilder builder;
            Json::Value root;
            std::string errors;
            std::istringstream iss(message);
            message = "";

            // Парсинг JSON
            if (!Json::parseFromStream(builder, iss, &root, &errors)) {
                std::cerr << "Error JSON: " << errors << std::endl;
                return 0;
            }
          
            // Проверка типа сообщения
            if (root.isMember("topic") && root["topic"].asString() == "publicTrade.BTCUSDT") {
                const Json::Value& data = root["data"];
             
                for (const auto& trade : data) {
                    system("cls");
                    std::cout << "\n";
                    std::cout << "     BTC/USDT:\n"
                        << "\n"
                        << "  Price: " << trade["p"].asString() << "\n"
                        << "  Volume: " << trade["v"].asString() << "\n" 
                        << "  Trade: " << trade["S"].asString() << "\n"
                        << "  Time: " << (timestamp_to_time(trade["T"].asUInt64())) << "\n" 
                        << "  ID trade: " << trade["i"].asString() << "\n"
                        << std::endl;
                    if (!message.empty()) break;
                  
                }
            }
                               
        }
        async_read.detach();
        ws.close(websocket::close_code::normal);
    }
    catch (std::exception const& e) {
      
        std::cerr << "Error: " << e.what() << std::endl;
        async_read.detach();       
        return EXIT_FAILURE;
    }
  
    return EXIT_SUCCESS;
}
