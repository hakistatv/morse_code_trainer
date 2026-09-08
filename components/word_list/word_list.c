/*
 * Bundled length-bucketed word list for Practice mode. See word_list.h.
 *
 * Buckets are plain NULL-terminated arrays of string literals, one per
 * length 1..6. Keep every entry uppercase A-Z and exactly as long as its
 * bucket -- components/word_list/test/ enforces both.
 */

#include <stddef.h>
#include "word_list.h"

static const char *const WORDS_1[] = {
    "A", "I",
    NULL,
};

static const char *const WORDS_2[] = {
    "AN", "AS", "AT", "BE", "BY", "DO", "GO", "HE", "IF", "IN",
    "IS", "IT", "ME", "MY", "NO", "OF", "ON", "OR", "SO", "TO",
    "UP", "US", "WE",
    NULL,
};

static const char *const WORDS_3[] = {
    "AND", "ARE", "BUT", "CAN", "CAT", "DAY", "DOG", "EAR", "EAT", "FAR",
    "FOR", "GET", "HAM", "HAS", "HER", "HIM", "HIS", "HOW", "ICE", "KEY",
    "MAN", "MAP", "NEW", "NOT", "NOW", "ONE", "OUR", "OUT", "RUN", "SEA",
    "SEE", "SUN", "THE", "TWO", "WAR", "WAS", "WHO", "WHY", "YES", "YOU",
    NULL,
};

static const char *const WORDS_4[] = {
    "ABLE", "BACK", "BAND", "BEAM", "BEST", "BIRD", "BOAT", "CALL", "CALM", "CODE",
    "COLD", "COME", "DARK", "DASH", "DATA", "DOWN", "EACH", "EASY", "FARM", "FAST",
    "FIRE", "FISH", "FROM", "GOLD", "GOOD", "HAND", "HEAR", "HELP", "HERE", "HOME",
    "IRON", "KEEN", "KIND", "LAKE", "LAND", "LIFE", "LINE", "LOUD", "MAKE", "MANY",
    "MOON", "NEAR", "NEWS", "NOTE", "OPEN", "OVER", "PATH", "RAIN", "READ", "ROAD",
    "SALT", "SEND", "SHIP", "SIGN", "SNOW", "STAR", "TALK", "TEAM", "TEST", "TIME",
    "TONE", "TREE", "WAVE", "WEST", "WIND", "WIRE", "WORD", "WORK", "ZERO",
    NULL,
};

static const char *const WORDS_5[] = {
    "AGENT", "ALARM", "APPLE", "BEACH", "BOARD", "BRAVE", "BREAD", "BRICK", "CABLE", "CHAIR",
    "CHART", "CLEAR", "CLOCK", "CLOUD", "COACH", "COAST", "DELTA", "DRINK", "EARLY", "EARTH",
    "FIELD", "FLAME", "FLASH", "FRAME", "GRASS", "GREEN", "HEART", "HORSE", "HOTEL", "HOUSE",
    "LIGHT", "MORSE", "MOTOR", "MUSIC", "NIGHT", "NOISE", "NORTH", "OCEAN", "PAPER", "PHONE",
    "PILOT", "PLANE", "POWER", "RADIO", "RIVER", "ROBOT", "SHORE", "SLEEP", "SOLAR", "SOUND",
    "SOUTH", "SPARK", "STONE", "STORM", "TABLE", "TIGER", "TOWER", "TRAIN", "WATCH", "WATER",
    "WHALE", "WHEEL", "WORLD",
    NULL,
};

static const char *const WORDS_6[] = {
    "ANCHOR", "BATTLE", "BEACON", "BRIDGE", "CAMERA", "CANDLE", "CASTLE", "CIRCLE", "COPPER", "DESERT",
    "DINNER", "DOCTOR", "ENERGY", "ENGINE", "FLIGHT", "FOREST", "FRIEND", "GARDEN", "GOLDEN", "GRAVEL",
    "GUITAR", "HAMMER", "HANDLE", "ISLAND", "JACKET", "JUNGLE", "LADDER", "LETTER", "MARKET", "MEADOW",
    "MIRROR", "MOMENT", "MOTHER", "NATURE", "ORANGE", "PACKET", "PADDLE", "PENCIL", "PLANET", "POCKET",
    "PUZZLE", "RADIOS", "RIDDLE", "ROCKET", "SADDLE", "SIGNAL", "SILVER", "SINGER", "SISTER", "SPRING",
    "SQUARE", "STREAM", "SUMMER", "SUNSET", "THRONE", "TIMBER", "TUNNEL", "VALLEY", "WINTER",
    NULL,
};

static const char *const *const BUCKETS[] = {
    WORDS_1, WORDS_2, WORDS_3, WORDS_4, WORDS_5, WORDS_6,
};

static const char *const *bucket_for(int len)
{
    if (len < WORD_LIST_MIN_LEN || len > WORD_LIST_MAX_LEN) {
        return NULL;
    }
    return BUCKETS[len - WORD_LIST_MIN_LEN];
}

int word_list_bucket_size(int len)
{
    const char *const *b = bucket_for(len);
    if (!b) {
        return 0;
    }
    int n = 0;
    while (b[n]) {
        n++;
    }
    return n;
}

const char *word_list_pick(int len, uint32_t rnd)
{
    const char *const *b = bucket_for(len);
    if (!b) {
        return NULL;
    }
    int n = word_list_bucket_size(len);
    if (n == 0) {
        return NULL;
    }
    return b[rnd % (uint32_t)n];
}
