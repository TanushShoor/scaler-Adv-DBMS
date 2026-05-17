#include <iostream>
#include <vector>
#include <cstdint>
#include <stdexcept>

template <typename T, typename V>
class ClockSweepCache
{
public:
    struct Frame
    {
        T key;
        V value;
        uint32_t usageCount{0};
        bool valid{false};
    };

    ClockSweepCache(uint32_t maxNumber)
    {   
        this->maxCacheSize = maxNumber;
        this->frames = std::vector<Frame>(maxNumber);
        if (maxCacheSize == 0)
        {
            throw std::invalid_argument("Cache size must be greater than zero");
        }
    }

    void putKey(T key, V value)
    {
        while (true)
        {
            Frame &fr = frames[clockHand];
            if (!fr.valid)
            {
                fr.key = key;
                fr.value = value;

                fr.valid = true;
                fr.usageCount = 1;

                updateClock();
                return;
            }

            if (fr.usageCount > 0)
            {
                fr.usageCount--;
            }
            else
            {
                fr.key = key;
                fr.value = value;

                fr.usageCount = 1;
                fr.valid = true;

                updateClock();
                return;
            }
            updateClock();
        }
    }

    bool getKey(T key, V &result)
    {
        for (Frame &fr : frames)
        {
            if (fr.valid && fr.key == key)
            {
                fr.usageCount++;
                result = fr.value;
                return true;
            }
        }
        return false;
    }

    void printState()
    {
        std::cout << "\nCache State:\n";\
        for (uint32_t i = 0; i < maxCacheSize; i++)
        {
            Frame &fr = frames[i];
            std::cout << "[" << i << "] ";
            if (fr.valid)
            {
                std::cout
                    << "Key=" << fr.key
                    << " Value=" << fr.value
                    << " Usage=" << fr.usageCount;
            }
            else
            {
                std::cout << "EMPTY";
            }

            if (i == clockHand)
            {
                std::cout << " <-- clock";
            }

            std::cout << "\n";
        }
    }

private:
    void updateClock()
    {
        clockHand = (clockHand + 1) % maxCacheSize;
    }

private:
    uint32_t maxCacheSize{0};
    uint32_t clockHand{0};
    std::vector<Frame> frames;
};

int main()
{
    ClockSweepCache<int, int> cache(3);

    cache.putKey(8, 100);
    cache.putKey(5, 200);
    cache.putKey(2, 300);

    cache.printState();

    int result;
    if (cache.getKey(1, result))
    {
        std::cout
            << "\nFound value: "
            << result
            << "\n";
    }

    cache.putKey(5, 400);

    cache.printState();

    return 0;
}