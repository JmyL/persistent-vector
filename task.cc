#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h> // for fsync()
#include <utility>
#include <vector>

static_assert(sizeof(size_t) == sizeof(uint64_t),
              "size_t and uint64_t are not the same size!");

constexpr unsigned long long operator"" _KB(unsigned long long num)
{
    return num * 1024;
}

int GetFileDescriptor(std::filebuf& filebuf)
{
    class my_filebuf : public std::filebuf
    {
    public:
        int handle() { return _M_file.fd(); }
    };

    // UBSan throw error because of downcasting.
    return static_cast<my_filebuf&>(filebuf).handle();
}

int GetFileDescriptor(std::ofstream& ofs)
{
    return GetFileDescriptor(*ofs.rdbuf());
}

template <typename T> T pad_to_multiple_of_8(T value)
{
    return (value + 7) & ~7;
}

/**
 * Persistent vector implementation.
 */
class vector
{
    struct alignas(8) Item
    {
        uint64_t id;
        std::string str;
        // std::stringstream oss;

        template <typename T>
        Item(uint64_t id, T&& str) : id{id}, str{std::forward<T>(str)}
        {
        }
        Item(const Item&) = default;
        Item(Item&&) = default;
        Item& operator=(const Item&) = default;
        Item& operator=(Item&&) = default;
    };

    struct Header
    {
        uint64_t type;
        uint64_t id;
        union
        {
            uint64_t dsize;
            uint64_t rindex;
        };
    };

public:
    /**
     * Create a new vector that can be persistent to `directory`.
     */
    vector(const std::filesystem::path& directory) : m_last_id(0)
    {
        std::filesystem::path filepath;
        filepath = directory / _filename;

        if (std::filesystem::exists(filepath))
        {
            load_from_file(filepath);
        }

        m_ofs.open(filepath, std::ios::binary | std::ios::app);
        // m_ofs << std::nounitbuf; //

        if (!m_ofs)
        {
            throw std::runtime_error("Failed to open " + filepath.string() +
                                     " for reading.");
        }

        m_bg_thread = std::jthread(
            [this](std::stop_token stoken)
            {
                std::unique_lock lock(m_mtx);
                while (!stoken.stop_requested())
                {
                    m_cv.wait_for( //
                        lock, std::chrono::milliseconds(1000),
                        [&]
                        {
                            // std::cout << "lambda start" << std::endl;
                            bool stop_requested = stoken.stop_requested();
                            if (!stop_requested)
                            {
                                m_ofs.flush();
                                // https://man7.org/linux/man-pages/man2/close.2.html
                                if (fsync(GetFileDescriptor(m_ofs)))
                                    exit(1);
                            }
                            // std::cout << "lambda end" << std::endl;
                            return stop_requested;
                        });
                    // std::cout << "end" << std::endl;
                    m_ofs.flush();
                    if (fsync(GetFileDescriptor(m_ofs)))
                        exit(1);
                }
            });
    }
    ~vector()
    {
        m_bg_thread.request_stop();
        m_cv.notify_all();
        m_bg_thread.join();
    }

    vector(const vector& v) = delete;
    vector& operator=(const vector& v) = delete;

    vector(vector&& v) noexcept { *this = std::move(v); }
    vector& operator=(vector&& v) noexcept
    {
        if (this != &v)
        {
            m_data = std::exchange(v.m_data, std::vector<Item>{});
        }
        return *this;
    }

    void push_back(const std::string& v)
    {
        uint64_t id;
        id = ++m_last_id;

        auto length = v.size();
        assert(length <= 4_KB);
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            // std::cout << "[push_back]" << std::endl;

            auto header = Header{.type = PUSHBACK, .id = id, .dsize = length};
            m_ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));
            m_ofs.write(v.data(), v.size());
            // m_ofs.flush();
        }
        periodic_notify(id);

        m_data.emplace_back(id, v);
    }

    std::string_view at(std::size_t index) const
    {
        return std::string_view(m_data.at(index).str);
    }

    void erase(std::size_t index)
    {
        auto it = m_data.begin() + index;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            // std::cout << "[erase]" << std::endl;
            auto header = Header{.type = ERASE, .id = it->id, .rindex = index};
            // m_ofs.flush();
            m_ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));
        }
        periodic_notify(++m_last_id);

        m_data.erase(it);
    }

    std::size_t size() const { return m_data.size(); }

private:
    static constexpr uint64_t PUSHBACK = 1;
    static constexpr uint64_t ERASE = 2;
    inline static constexpr const char* _filename = ".vector.bin";
    std::vector<Item> m_data;
    std::ofstream m_ofs;
    std::uint64_t m_last_id;

    // Background thread
    std::jthread m_bg_thread;
    std::condition_variable m_cv;
    std::mutex m_mtx;

    void periodic_notify(uint64_t id)
    {
        if ((id & 0xFF) == 0)
        {
            m_cv.notify_all();
        }
    }
    void load_from_file(std::filesystem::path& filepath)
    {
        auto ifs = std::ifstream(filepath, std::ios::binary);
        if (!ifs)
        {
            throw std::runtime_error("Failed to open " + filepath.string() +
                                     " for reading.");
        }
        // Stop loading if file has an error
        while (true)
        {
            Header header;
            ifs.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!ifs)
                break;

            if (header.type == ERASE)
            {
                assert(m_data.at(header.rindex).id == header.id);
                m_data.erase(m_data.begin() + header.rindex);
            }
            else if (header.type == PUSHBACK)
            {
                auto str = std::string();
                str.resize(header.dsize);
                ifs.read(str.data(), header.dsize);
                if (!ifs)
                    break;

                m_data.emplace_back(header.id, str);
            }
        }
    }
};

std::size_t errors = 0;

#define ERROR(msg)                                                             \
    {                                                                          \
        std::cout << __FILE__ << ":" << __LINE__ << " " << msg << "\n";        \
        ++errors;                                                              \
    }
#define CHECK(x)                                                               \
    do                                                                         \
        if (!(x))                                                              \
        {                                                                      \
            ERROR(#x " failed");                                               \
        }                                                                      \
    while (false)

constexpr unsigned LOOP_COUNT = 100000;

std::string all_chars()
{
    std::string rv;
    for (char c = std::numeric_limits<char>::min();
         c != std::numeric_limits<char>::max(); ++c)
    {
        rv += c;
    }

    rv += std::numeric_limits<char>::max();
    return rv;
}

std::string chars_4K(char v)
{
    std::string rv(4_KB, v);
    return rv;
}

void run_test_one(const std::filesystem::path& p)
{
    vector v(p);
    using namespace std::literals;

    v.push_back("foo");
    CHECK(v.at(0) == "foo");
    CHECK(v.size() == 1);

    v.push_back(all_chars());
    CHECK(v.at(1) == all_chars());
    CHECK(v.size() == 2);

    auto start = std::chrono::system_clock::now();
    for (auto i = 0u; i < LOOP_COUNT; ++i)
    {
        std::stringstream s;
        s << "loop " << i;
        v.push_back(s.str());
    }
    auto end = std::chrono::system_clock::now();
    std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(end -
                                                                       start)
                     .count()
              << " ms" << std::endl;
    CHECK((end - start) / 1s < 1);
    CHECK(v.size() == LOOP_COUNT + 2);
}

void run_test_two(const std::filesystem::path& p)
{
    vector v(p);

    CHECK(v.size() == LOOP_COUNT + 2);
    CHECK(v.at(0) == "foo");
    CHECK(v.at(1) == all_chars());
    CHECK(v.at(873) == "loop 871");

    v.erase(873);
    CHECK(v.size() == LOOP_COUNT + 1);
    CHECK(v.at(0) == "foo");
    CHECK(v.at(1) == all_chars());
    CHECK(v.at(873) == "loop 872");
}

void run_test_three(const std::filesystem::path& p)
{
    vector v(p);

    CHECK(v.size() == LOOP_COUNT + 1);
    CHECK(v.at(0) == "foo");
    CHECK(v.at(1) == all_chars());
    CHECK(v.at(873) == "loop 872");

    v.erase(873);
    CHECK(v.size() == LOOP_COUNT);
    CHECK(v.at(0) == "foo");
    CHECK(v.at(1) == all_chars());
    CHECK(v.at(873) == "loop 873");
}

void run_test_four(const std::filesystem::path& p)
{
    vector v(p);
    using namespace std::literals;

    while (auto i = v.size())
    {
        v.erase(i - 1);
    }

    auto start = std::chrono::system_clock::now();
    for (auto i = 0u; i < LOOP_COUNT; ++i)
    {
        v.push_back(chars_4K(i));
        CHECK(v.at(i) == chars_4K(i));
        CHECK(v.size() == i + 1);
    }
    auto end = std::chrono::system_clock::now();
    std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(end -
                                                                       start)
                     .count()
              << " ms" << std::endl;
    CHECK((end - start) / 1s < 1);
    CHECK(v.size() == LOOP_COUNT);
}

int main(int argc, char**)
{
    std::filesystem::path data_dir("data_dir");

    try
    {
        if (std::filesystem::exists(data_dir))
        {
            std::filesystem::remove_all(data_dir);
            std::cout << "Directory removed.\n";
        }
        else
        {
            std::cout << "Directory does not exist.\n";
        }
    }
    catch (const std::filesystem::filesystem_error& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    std::filesystem::create_directory(data_dir);

    run_test_one(data_dir);
    run_test_two(data_dir);
    run_test_three(data_dir);
    run_test_four(data_dir);

    if (errors != 0)
    {
        std::cout << "tests were failing\n";
        return 1;
    }

    std::cout << "tests succeeded\n";
    return 0;
}
