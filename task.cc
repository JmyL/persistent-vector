#include <cassert>
#include <fcntl.h> // for open()
#include <filesystem>
#include <fstream> // for ifstream
#include <iostream>
#include <limits>
#include <string>
#include <unistd.h> // for fsync()
#include <utility>
#include <vector>

#include "lockfree_queue.hh"

constexpr unsigned long long operator"" _KB(unsigned long long num)
{
    return num * 1024;
}

uint64_t xor_checksum64(const uint64_t* data, size_t length)
{
    uint64_t checksum = 0;
    for (size_t i = 0; i < length; ++i)
    {
        checksum ^= data[i];
    }
    return checksum;
}

/**
 * Persistent vector implementation.
 */
class vector
{
    struct item
    {
        std::uint64_t id;
        std::string str;

        template <typename T>
        item(std::uint64_t id, T&& str) : id{id}, str{std::forward<T>(str)}
        {
        }
    };

public:
    /**
     * Create a new vector that can be persistent to `directory`.
     */
    vector(const std::filesystem::path& directory) : _last_id(0)
    {
        _filepath = directory / _filename;

        if (std::filesystem::exists(_filepath))
        {
            if (auto _infile = std::ifstream(_filepath, std::ios::binary);
                !_infile)
            {
                throw std::runtime_error("Failed to open " +
                                         _filepath.string() + " for reading.");
            }
            else
            {
                // TODO:load file to internal vector, _last_id and _holes
                // If the file has an error, stop conversion and remove
                // corrupted information.
                while (true)
                {
                    std::uint64_t id;
                    _infile.read(reinterpret_cast<char*>(&id), sizeof(id));
                    if (_infile.eof())
                    {
                        break;
                    }
                    std::int16_t size;
                    _infile.read(reinterpret_cast<char*>(&size), sizeof(size));
                    if (_infile.eof())
                    {
                        break;
                    }
                    if (size == _tombstone)
                    {
                        std::size_t index;
                        _infile.read(reinterpret_cast<char*>(&index),
                                     sizeof(index));
                        if (_infile.eof())
                        {
                            break;
                        }
                        assert(_data.at(index).id == id);
                        _holes.push(id);
                        _data.erase(_data.begin() + index);
                    }
                    else
                    {
                        std::string str;
                        str.resize(size);
                        _infile.read(&str[0],
                                     size); // 문자열 데이터를 읽어와 저장
                        if (_infile.eof())
                        {
                            break;
                        }
                        _data.emplace_back(id, str);
                    }
                }

                _infile.close();
            }
        }

        // if (_file =
        //         std::ofstream(_filepath, std::ios::binary | std::ios::trunc);
        //     !_file)
        if (_file = std::ofstream(_filepath, std::ios::binary | std::ios::app);
            !_file)
        {
            throw std::runtime_error("Failed to open " + _filepath.string() +
                                     " for reading.");
        }
        else
        {
        }
    }
    ~vector() { _file.close(); }

    vector(const vector& v) = delete;
    vector& operator=(const vector& v) = delete;

    vector(vector&& v) noexcept { *this = std::move(v); }
    vector& operator=(vector&& v) noexcept
    {
        if (this != &v)
        {
            _data = std::exchange(v._data, std::vector<item>{});
        }
        return *this;
    }

    void push_back(const std::string& v)
    {
        std::uint64_t id;
        if (!_holes.empty())
        {
            id = _holes.front();
            _holes.pop();
        }
        else
        {
            id = ++_last_id;
        }
        _data.emplace_back(id, v);

        size_t length = v.size();
        assert(length <= 4_KB);
        assert(length < std::numeric_limits<std::int16_t>::max());
        // Write, even if size is zero
        std::int16_t truncated_length = static_cast<std::int16_t>(length);
        _file.write(reinterpret_cast<const char*>(&id), sizeof(id));
        _file.write(reinterpret_cast<const char*>(&truncated_length),
                    sizeof(truncated_length));
        _file.write(v.data(), truncated_length);
    }

    std::string_view at(std::size_t index) const
    {
        return std::string_view(_data.at(index).str);
    }

    void erase(std::size_t index)
    {
        auto it = _data.begin() + index;
        _holes.push(it->id);

        _file.write(reinterpret_cast<const char*>(&it->id), sizeof(it->id));
        _file.write(reinterpret_cast<const char*>(&_tombstone),
                    sizeof(_tombstone));
        _file.write(reinterpret_cast<const char*>(&index), sizeof(index));

        _data.erase(it);
    }

    std::size_t size() const { return _data.size(); }

private:
    inline static constexpr const char* _filename = ".vector.bin";
    inline static constexpr const std::int16_t _tombstone = -1;
    std::vector<item> _data;
    std::filesystem::path _filepath;
    std::ofstream _file;
    std::uint64_t _last_id;
    std::queue<size_t> _holes;
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
            std::uintmax_t num_removed = std::filesystem::remove_all(
                data_dir); // 폴더와 내부 파일 및 하위 디렉토리 삭제
            std::cout << num_removed << " items removed.\n";
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
