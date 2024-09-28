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
        m_filepath = directory / _filename;

        if (std::filesystem::exists(m_filepath))
        {
            load_from_file(m_filepath);
        }

        m_ofs.open(m_filepath,
                   std::ios::out | std::ios::binary | std::ios::app);

        if (!m_ofs.is_open())
        {
            throw std::runtime_error("Failed to open " + m_filepath.string() +
                                     " for reading.");
        }

        m_bg_thread = std::jthread(
            [this](std::stop_token stoken)
            {
                std::mutex mtx;
                std::unique_lock lock(mtx);
                while (!stoken.stop_requested())
                {
                    cv.wait_for(lock, std::chrono::milliseconds(1000),
                                [&stoken] { return stoken.stop_requested(); });
                    m_ofs.flush();
                    fsync(GetFileDescriptor(m_ofs));
                    std::cout << "thread..." << std::endl;
                }
            });
    }
    ~vector()
    {
        m_bg_thread.request_stop();
        cv.notify_all();
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
            auto header = Header{.type = PUSHBACK, .id = id, .dsize = length};
            m_ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));
            m_ofs << v;
        }

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
            auto header = Header{.type = ERASE, .id = it->id, .rindex = index};
            m_ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));
        }

        m_data.erase(it);
    }

    std::size_t size() const { return m_data.size(); }

private:
    static constexpr uint64_t PUSHBACK = 1;
    static constexpr uint64_t ERASE = 2;
    inline static constexpr const char* _filename = ".vector.bin";
    std::vector<Item> m_data;
    std::filesystem::path m_filepath;
    std::ofstream m_ofs;
    std::uint64_t m_last_id;
    std::jthread m_bg_thread;
    std::condition_variable cv;
    std::mutex m_mtx;

    void load_from_file(std::filesystem::path& m_filepath)
    {
        auto _infile = std::ifstream(m_filepath, std::ios::binary);
        if (!_infile)
        {
            throw std::runtime_error("Failed to open " + m_filepath.string() +
                                     " for reading.");
        }
        // TODO:load file to internal vector, m_last_id.
        // If the file has an error, stop conversion and remove
        // corrupted information.
        while (true)
        {
            Header header;
            _infile.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (_infile.eof())
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
                _infile.read(reinterpret_cast<char*>(str.data()), header.dsize);
                if (_infile.eof())
                    break;

                m_data.emplace_back(header.id, str);
            }

            _infile.close();
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
    CHECK((end - start) / 1s < 1);
    CHECK(v.size() == LOOP_COUNT + 2);
    std::this_thread::sleep_for(1s);
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

    if (errors != 0)
    {
        std::cout << "tests were failing\n";
        return 1;
    }

    std::cout << "tests succeeded\n";
    return 0;
}
