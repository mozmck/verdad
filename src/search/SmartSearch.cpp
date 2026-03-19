#include "search/SmartSearch.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace verdad {
namespace smart_search {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ─── Diacritics stripping ────────────────────────────────────────────
// Maps two-byte UTF-8 sequences (Latin-1 Supplement and Latin Extended-A/B)
// to their base ASCII character.  This covers the accented characters used
// in Spanish, Portuguese, French, German, and other European languages
// commonly seen in Bible module metadata and content.

// Returns the ASCII base character for a 2-byte UTF-8 codepoint, or 0 if
// the codepoint is not a recognized accented Latin letter.
char mapTwoByteToAscii(unsigned char b0, unsigned char b1) {
    // UTF-8 two-byte: 110xxxxx 10xxxxxx  →  codepoint = (b0 & 0x1F) << 6 | (b1 & 0x3F)
    unsigned int cp = (static_cast<unsigned int>(b0 & 0x1F) << 6) |
                       static_cast<unsigned int>(b1 & 0x3F);

    // Latin-1 Supplement (U+00C0 – U+00FF)
    switch (cp) {
    // A variants
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return 'A';
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return 'a';
    // AE ligature
    case 0xC6: return 'A'; // Æ → A (close enough for search)
    case 0xE6: return 'a'; // æ → a
    // C variants
    case 0xC7: return 'C'; // Ç
    case 0xE7: return 'c'; // ç
    // D variants
    case 0xD0: return 'D'; // Ð
    case 0xF0: return 'd'; // ð
    // E variants
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return 'E';
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
    // I variants
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: return 'I';
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return 'i';
    // N variants
    case 0xD1: return 'N'; // Ñ
    case 0xF1: return 'n'; // ñ
    // O variants
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return 'O';
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return 'o';
    // U variants
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: return 'U';
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'u';
    // Y variants
    case 0xDD: return 'Y'; // Ý
    case 0xFD: case 0xFF: return 'y'; // ý, ÿ
    // Thorn
    case 0xDE: return 'T'; // Þ
    case 0xFE: return 't'; // þ
    // Sharp S
    case 0xDF: return 's'; // ß → ss, but single 's' is fine for search
    default: break;
    }

    // Latin Extended-A (U+0100 – U+017F) — common ones
    switch (cp) {
    case 0x100: case 0x102: case 0x104: return 'A';
    case 0x101: case 0x103: case 0x105: return 'a';
    case 0x106: case 0x108: case 0x10A: case 0x10C: return 'C';
    case 0x107: case 0x109: case 0x10B: case 0x10D: return 'c';
    case 0x10E: case 0x110: return 'D';
    case 0x10F: case 0x111: return 'd';
    case 0x112: case 0x114: case 0x116: case 0x118: case 0x11A: return 'E';
    case 0x113: case 0x115: case 0x117: case 0x119: case 0x11B: return 'e';
    case 0x11C: case 0x11E: case 0x120: case 0x122: return 'G';
    case 0x11D: case 0x11F: case 0x121: case 0x123: return 'g';
    case 0x124: case 0x126: return 'H';
    case 0x125: case 0x127: return 'h';
    case 0x128: case 0x12A: case 0x12C: case 0x12E: case 0x130: return 'I';
    case 0x129: case 0x12B: case 0x12D: case 0x12F: case 0x131: return 'i';
    case 0x134: return 'J';
    case 0x135: return 'j';
    case 0x136: return 'K';
    case 0x137: return 'k';
    case 0x139: case 0x13B: case 0x13D: case 0x13F: case 0x141: return 'L';
    case 0x13A: case 0x13C: case 0x13E: case 0x140: case 0x142: return 'l';
    case 0x143: case 0x145: case 0x147: return 'N';
    case 0x144: case 0x146: case 0x148: case 0x149: return 'n';
    case 0x14C: case 0x14E: case 0x150: return 'O';
    case 0x14D: case 0x14F: case 0x151: return 'o';
    case 0x152: return 'O'; // Œ
    case 0x153: return 'o'; // œ
    case 0x154: case 0x156: case 0x158: return 'R';
    case 0x155: case 0x157: case 0x159: return 'r';
    case 0x15A: case 0x15C: case 0x15E: case 0x160: return 'S';
    case 0x15B: case 0x15D: case 0x15F: case 0x161: return 's';
    case 0x162: case 0x164: case 0x166: return 'T';
    case 0x163: case 0x165: case 0x167: return 't';
    case 0x168: case 0x16A: case 0x16C: case 0x16E: case 0x170: case 0x172: return 'U';
    case 0x169: case 0x16B: case 0x16D: case 0x16F: case 0x171: case 0x173: return 'u';
    case 0x174: return 'W';
    case 0x175: return 'w';
    case 0x176: case 0x178: return 'Y';
    case 0x177: return 'y';
    case 0x179: case 0x17B: case 0x17D: return 'Z';
    case 0x17A: case 0x17C: case 0x17E: return 'z';
    default: break;
    }

    return 0;
}

std::string doStripDiacritics(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char b = static_cast<unsigned char>(text[i]);

        // Plain ASCII — pass through
        if (b < 0x80) {
            out.push_back(text[i]);
            continue;
        }

        // Two-byte UTF-8 sequence (covers U+0080 – U+07FF)
        if ((b & 0xE0) == 0xC0 && i + 1 < text.size()) {
            unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
            if ((b1 & 0xC0) == 0x80) {
                char mapped = mapTwoByteToAscii(b, b1);
                if (mapped) {
                    out.push_back(mapped);
                    ++i;
                    continue;
                }
                // Unrecognized 2-byte — keep as-is
                out.push_back(text[i]);
                out.push_back(text[i + 1]);
                ++i;
                continue;
            }
        }

        // Three-byte UTF-8 (covers U+0800 – U+FFFF) — pass through
        if ((b & 0xF0) == 0xE0 && i + 2 < text.size()) {
            out.push_back(text[i]);
            out.push_back(text[i + 1]);
            out.push_back(text[i + 2]);
            i += 2;
            continue;
        }

        // Four-byte UTF-8 — pass through
        if ((b & 0xF8) == 0xF0 && i + 3 < text.size()) {
            out.push_back(text[i]);
            out.push_back(text[i + 1]);
            out.push_back(text[i + 2]);
            out.push_back(text[i + 3]);
            i += 3;
            continue;
        }

        // Fallback — keep byte
        out.push_back(text[i]);
    }

    return out;
}

// ─── Synonym database ────────────────────────────────────────────────
// Compact embedded synonym groups for Biblical/theological English.
// Each group is a set of words that are interchangeable in meaning.
// The database focuses on terms commonly found in Bible translations
// where different versions use different words for the same concept.

struct SynonymGroup {
    std::vector<std::string> words;
};

// English synonym groups - Biblical/theological vocabulary
const std::vector<SynonymGroup>& englishSynonyms() {
    static const std::vector<SynonymGroup> groups = {
        // Deity / divine references
        {{"god", "lord", "almighty", "creator", "deity"}},
        {{"jesus", "christ", "messiah", "savior", "saviour", "redeemer", "immanuel", "emmanuel"}},
        {{"holy spirit", "spirit", "comforter", "counselor", "counsellor", "advocate", "helper", "paraclete"}},

        // People / roles
        {{"king", "ruler", "sovereign", "monarch"}},
        {{"queen", "empress", "sovereign"}},
        {{"prophet", "seer", "oracle"}},
        {{"priest", "minister", "clergyman"}},
        {{"apostle", "disciple", "follower"}},
        {{"servant", "slave", "bondservant", "bondman", "handmaid", "handmaiden", "maidservant"}},
        {{"master", "lord", "ruler", "owner"}},
        {{"teacher", "rabbi", "master", "instructor"}},
        {{"shepherd", "pastor", "herdsman"}},
        {{"child", "children", "offspring", "progeny", "son", "daughter"}},
        {{"father", "dad", "parent", "patriarch"}},
        {{"mother", "parent", "matriarch"}},
        {{"brother", "sibling", "brethren", "kinsman"}},
        {{"wife", "spouse", "bride", "woman"}},
        {{"husband", "spouse", "bridegroom", "man"}},
        {{"enemy", "foe", "adversary", "opponent"}},
        {{"stranger", "foreigner", "alien", "sojourner", "pilgrim"}},
        {{"elder", "overseer", "bishop"}},
        {{"warrior", "soldier", "fighter", "champion"}},
        {{"nation", "people", "gentile", "heathen"}},

        // Actions / verbs
        {{"pray", "prayer", "supplication", "petition", "intercession", "entreat", "beseech", "implore"}},
        {{"worship", "praise", "adore", "glorify", "exalt", "magnify", "honor", "honour", "venerate"}},
        {{"sin", "transgression", "iniquity", "trespass", "wickedness", "offense", "offence", "wrongdoing"}},
        {{"repent", "repentance", "atone", "atonement", "contrition"}},
        {{"forgive", "forgiveness", "pardon", "absolve", "remit", "remission"}},
        {{"save", "salvation", "deliver", "deliverance", "rescue", "redeem", "redemption"}},
        {{"bless", "blessing", "benediction", "beatitude"}},
        {{"curse", "cursed", "accursed", "damned", "anathema"}},
        {{"believe", "faith", "trust", "confidence"}},
        {{"love", "charity", "compassion", "lovingkindness", "loving-kindness", "kindness", "mercy", "affection"}},
        {{"hate", "hatred", "abhor", "detest", "loathe", "despise"}},
        {{"fear", "dread", "terror", "trembling", "awe", "reverence"}},
        {{"rejoice", "joy", "gladness", "delight", "happiness", "mirth", "jubilation"}},
        {{"weep", "cry", "mourn", "lament", "wail", "grieve", "sorrow"}},
        {{"heal", "cure", "restore", "mend", "recover"}},
        {{"destroy", "demolish", "devastate", "annihilate", "ruin", "desolate"}},
        {{"build", "construct", "erect", "establish"}},
        {{"die", "death", "perish", "expire", "decease"}},
        {{"kill", "slay", "murder", "smite", "slaughter", "execute"}},
        {{"fight", "battle", "war", "combat", "conflict", "warfare", "strife"}},
        {{"speak", "say", "tell", "declare", "proclaim", "announce", "utter"}},
        {{"hear", "listen", "hearken", "heed", "attend"}},
        {{"see", "behold", "observe", "perceive", "witness", "look"}},
        {{"know", "understand", "comprehend", "perceive", "discern"}},
        {{"teach", "instruct", "educate", "train", "admonish", "counsel"}},
        {{"judge", "judgment", "judgement", "verdict", "justice", "adjudicate"}},
        {{"command", "commandment", "decree", "statute", "ordinance", "precept", "mandate", "edict"}},
        {{"obey", "obedience", "submit", "submission", "comply"}},
        {{"rebel", "rebellion", "revolt", "disobey", "disobedience", "defiance"}},
        {{"give", "offer", "present", "bestow", "grant", "donate"}},
        {{"receive", "accept", "take", "obtain", "acquire"}},
        {{"send", "dispatch", "commission", "appoint"}},
        {{"come", "arrive", "approach", "draw near"}},
        {{"go", "depart", "leave", "journey", "travel"}},
        {{"rise", "arise", "stand", "resurrect", "resurrection"}},
        {{"fall", "stumble", "collapse", "descend"}},
        {{"create", "creation", "make", "form", "fashion", "mold", "mould"}},
        {{"dwell", "abide", "reside", "live", "inhabit", "sojourn"}},
        {{"gather", "assemble", "congregate", "collect"}},
        {{"scatter", "disperse", "spread", "disseminate"}},
        {{"anoint", "consecrate", "dedicate", "sanctify", "hallow"}},

        // Concepts / nouns
        {{"heaven", "paradise", "sky", "firmament"}},
        {{"hell", "hades", "sheol", "gehenna", "abyss", "pit", "underworld"}},
        {{"earth", "world", "land", "ground", "soil"}},
        {{"sea", "ocean", "waters", "deep"}},
        {{"mountain", "mount", "hill", "peak"}},
        {{"river", "stream", "brook", "creek", "torrent", "wadi"}},
        {{"desert", "wilderness", "wasteland", "barren"}},
        {{"temple", "sanctuary", "tabernacle", "shrine", "house of god"}},
        {{"church", "congregation", "assembly", "ecclesia"}},
        {{"covenant", "promise", "agreement", "pact", "testament"}},
        {{"law", "torah", "statute", "decree", "commandment", "ordinance"}},
        {{"gospel", "good news", "glad tidings", "message"}},
        {{"grace", "favor", "favour", "kindness", "mercy", "clemency"}},
        {{"glory", "splendor", "splendour", "majesty", "radiance", "brilliance"}},
        {{"righteous", "righteousness", "just", "justice", "upright", "virtuous"}},
        {{"wicked", "evil", "ungodly", "sinful", "iniquitous", "corrupt"}},
        {{"holy", "sacred", "sanctified", "hallowed", "consecrated", "divine"}},
        {{"pure", "clean", "spotless", "blameless", "innocent", "unblemished"}},
        {{"power", "might", "strength", "authority", "dominion", "sovereignty"}},
        {{"wisdom", "understanding", "knowledge", "insight", "discernment", "prudence"}},
        {{"peace", "tranquility", "shalom", "serenity", "harmony", "rest"}},
        {{"hope", "expectation", "anticipation", "confidence"}},
        {{"truth", "verity", "honesty", "faithfulness", "fidelity"}},
        {{"light", "radiance", "brilliance", "illumination", "luminance"}},
        {{"darkness", "shadow", "gloom", "blackness", "night"}},
        {{"life", "living", "existence", "vitality"}},
        {{"bread", "food", "nourishment", "sustenance", "provision"}},
        {{"wine", "drink", "cup", "libation"}},
        {{"blood", "sacrifice", "offering", "atonement"}},
        {{"cross", "crucify", "crucifixion", "calvary", "golgotha"}},
        {{"angel", "messenger", "seraph", "seraphim", "cherub", "cherubim"}},
        {{"demon", "devil", "satan", "evil spirit", "unclean spirit", "fiend"}},
        {{"miracle", "wonder", "sign", "marvel", "portent", "prodigy"}},
        {{"prophecy", "revelation", "vision", "oracle"}},
        {{"parable", "allegory", "fable", "story", "illustration"}},
        {{"tithe", "offering", "sacrifice", "oblation", "gift"}},
        {{"baptize", "baptism", "immerse", "immersion", "washing", "cleansing"}},
        {{"eternal", "everlasting", "forever", "perpetual", "immortal", "infinite"}},
        {{"tribulation", "trial", "affliction", "suffering", "persecution", "distress", "hardship"}},
        {{"idol", "idolatry", "graven image", "false god", "abomination"}},
        {{"treasure", "riches", "wealth", "abundance", "prosperity"}},
        {{"poor", "poverty", "needy", "destitute", "humble", "lowly", "meek"}},
        {{"proud", "pride", "arrogant", "arrogance", "haughty", "vain"}},
        {{"faithful", "faithful", "loyal", "steadfast", "devoted", "true"}},
        {{"wrath", "anger", "fury", "rage", "indignation"}},
        {{"patience", "endurance", "perseverance", "longsuffering", "forbearance"}},
        {{"temptation", "test", "trial", "enticement", "allurement"}},
        {{"armor", "armour", "shield", "breastplate", "helmet", "sword"}},

        // Nature / animals
        {{"lamb", "sheep", "ewe", "ram", "flock"}},
        {{"lion", "beast", "predator"}},
        {{"dove", "pigeon", "turtledove"}},
        {{"serpent", "snake", "viper", "dragon"}},
        {{"ox", "bull", "bullock", "calf", "cattle", "herd"}},
        {{"horse", "steed", "chariot"}},
        {{"fig", "olive", "vine", "vineyard"}},
        {{"tree", "cedar", "oak", "palm"}},
        {{"garden", "orchard", "grove"}},
        {{"seed", "grain", "wheat", "barley", "harvest", "crop"}},

        // Time
        {{"day", "morning", "dawn", "daybreak", "sunrise"}},
        {{"night", "evening", "dusk", "sunset", "twilight"}},
        {{"sabbath", "rest", "seventh day"}},
        {{"feast", "festival", "celebration", "passover"}},

        // Places
        {{"jerusalem", "zion", "sion", "city of david", "holy city"}},
        {{"egypt", "land of pharaoh"}},
        {{"babylon", "babel", "chaldea"}},

        // Misc archaic/modern equivalents
        {{"behold", "look", "see", "observe", "lo"}},
        {{"verily", "truly", "indeed", "certainly", "surely", "assuredly"}},
        {{"thou", "you", "thee", "ye"}},
        {{"thy", "your", "thine"}},
        {{"hath", "has", "have"}},
        {{"doth", "does", "do"}},
        {{"saith", "says", "said", "spoke"}},
        {{"cometh", "comes", "came", "arrived"}},
        {{"goeth", "goes", "went", "departed"}},
        {{"giveth", "gives", "gave"}},
        {{"maketh", "makes", "made"}},
        {{"taketh", "takes", "took"}},
        {{"knoweth", "knows", "knew"}},
        {{"loveth", "loves", "loved"}},
        {{"liveth", "lives", "lived"}},
        {{"seeketh", "seeks", "sought"}},
        {{"doeth", "does", "did"}},
        {{"hast", "have", "had"}},
        {{"shalt", "shall", "will"}},
        {{"unto", "to", "toward", "towards"}},
        {{"thereof", "of it", "its"}},
        {{"therein", "in it", "within"}},
        {{"wherein", "in which", "where"}},
        {{"wherefore", "therefore", "why", "for what reason"}},
        {{"whence", "where from", "from where"}},
        {{"hither", "here", "to this place"}},
        {{"thither", "there", "to that place"}},
        {{"midst", "middle", "center", "centre", "among"}},
        {{"nay", "no", "not"}},
        {{"yea", "yes", "indeed", "truly"}},
        {{"thus", "so", "therefore", "accordingly"}},
        {{"ere", "before"}},
        {{"hence", "from here", "therefore"}},
        {{"whilst", "while", "during"}},
    };
    return groups;
}

// Spanish synonym groups - Biblical vocabulary
const std::vector<SynonymGroup>& spanishSynonyms() {
    static const std::vector<SynonymGroup> groups = {
        {{"dios", "señor", "todopoderoso", "creador"}},
        {{"jesús", "cristo", "mesías", "salvador", "redentor"}},
        {{"espíritu santo", "espíritu", "consolador", "consejero"}},
        {{"rey", "gobernante", "soberano", "monarca"}},
        {{"profeta", "vidente", "oráculo"}},
        {{"sacerdote", "ministro"}},
        {{"apóstol", "discípulo", "seguidor"}},
        {{"siervo", "esclavo", "criado"}},
        {{"orar", "oración", "súplica", "petición", "intercesión", "rogar"}},
        {{"adorar", "alabar", "glorificar", "exaltar", "honrar", "venerar"}},
        {{"pecado", "transgresión", "iniquidad", "maldad", "ofensa"}},
        {{"arrepentir", "arrepentimiento", "contrición"}},
        {{"perdonar", "perdón", "absolver", "remisión"}},
        {{"salvar", "salvación", "liberar", "liberación", "rescatar", "redimir", "redención"}},
        {{"bendecir", "bendición"}},
        {{"maldecir", "maldición", "anatema"}},
        {{"creer", "fe", "confianza"}},
        {{"amor", "caridad", "compasión", "misericordia", "bondad", "clemencia"}},
        {{"gozo", "alegría", "regocijo", "felicidad", "júbilo"}},
        {{"llorar", "lamentar", "gemir", "afligirse", "dolor"}},
        {{"sanar", "curar", "restaurar"}},
        {{"destruir", "demoler", "devastar", "aniquilar", "ruina", "desolación"}},
        {{"morir", "muerte", "perecer", "fallecer"}},
        {{"matar", "asesinar", "herir"}},
        {{"hablar", "decir", "declarar", "proclamar", "anunciar"}},
        {{"oír", "escuchar", "atender"}},
        {{"ver", "mirar", "observar", "percibir", "contemplar"}},
        {{"saber", "entender", "comprender", "percibir", "discernir", "conocer"}},
        {{"cielo", "paraíso", "firmamento"}},
        {{"infierno", "hades", "seol", "abismo"}},
        {{"tierra", "mundo", "suelo"}},
        {{"templo", "santuario", "tabernáculo"}},
        {{"iglesia", "congregación", "asamblea"}},
        {{"pacto", "alianza", "promesa", "acuerdo", "testamento"}},
        {{"ley", "estatuto", "decreto", "mandamiento", "ordenanza"}},
        {{"evangelio", "buenas nuevas", "mensaje"}},
        {{"gracia", "favor", "bondad", "misericordia"}},
        {{"gloria", "esplendor", "majestad", "resplandor"}},
        {{"justo", "justicia", "recto", "virtuoso"}},
        {{"malo", "malvado", "impío", "pecaminoso", "corrupto", "inicuo"}},
        {{"santo", "sagrado", "santificado", "consagrado", "divino"}},
        {{"puro", "limpio", "inocente", "inmaculado"}},
        {{"poder", "fuerza", "autoridad", "dominio", "soberanía"}},
        {{"sabiduría", "entendimiento", "conocimiento", "discernimiento", "prudencia"}},
        {{"paz", "tranquilidad", "serenidad", "armonía", "descanso"}},
        {{"esperanza", "expectativa", "confianza"}},
        {{"verdad", "honestidad", "fidelidad"}},
        {{"luz", "resplandor", "iluminación"}},
        {{"oscuridad", "tinieblas", "sombra", "noche"}},
        {{"vida", "vivir", "existencia"}},
        {{"eterno", "perpetuo", "inmortal", "infinito", "sempiterno"}},
        {{"ángel", "mensajero", "serafín", "querubín"}},
        {{"demonio", "diablo", "satanás", "espíritu inmundo"}},
        {{"milagro", "maravilla", "señal", "prodigio"}},
        {{"profecía", "revelación", "visión"}},
        {{"bautizar", "bautismo", "inmersión"}},
    };
    return groups;
}

// Portuguese synonym groups
const std::vector<SynonymGroup>& portugueseSynonyms() {
    static const std::vector<SynonymGroup> groups = {
        {{"deus", "senhor", "todo-poderoso", "criador"}},
        {{"jesus", "cristo", "messias", "salvador", "redentor"}},
        {{"espírito santo", "espírito", "consolador", "conselheiro"}},
        {{"rei", "governante", "soberano"}},
        {{"profeta", "vidente", "oráculo"}},
        {{"sacerdote", "ministro"}},
        {{"apóstolo", "discípulo", "seguidor"}},
        {{"servo", "escravo", "criado"}},
        {{"orar", "oração", "súplica", "petição", "intercessão", "rogar"}},
        {{"adorar", "louvar", "glorificar", "exaltar", "honrar"}},
        {{"pecado", "transgressão", "iniquidade", "maldade", "ofensa"}},
        {{"arrepender", "arrependimento", "contrição"}},
        {{"perdoar", "perdão", "absolver", "remissão"}},
        {{"salvar", "salvação", "libertar", "libertação", "resgatar", "remir", "redenção"}},
        {{"abençoar", "bênção"}},
        {{"amaldiçoar", "maldição"}},
        {{"crer", "fé", "confiança"}},
        {{"amor", "caridade", "compaixão", "misericórdia", "bondade"}},
        {{"alegria", "gozo", "regozijo", "felicidade", "júbilo"}},
        {{"chorar", "lamentar", "gemer", "afligir", "dor"}},
        {{"curar", "sarar", "restaurar"}},
        {{"morrer", "morte", "perecer", "falecer"}},
        {{"matar", "assassinar", "ferir"}},
        {{"falar", "dizer", "declarar", "proclamar", "anunciar"}},
        {{"ouvir", "escutar", "atender"}},
        {{"ver", "olhar", "observar", "perceber", "contemplar"}},
        {{"céu", "paraíso", "firmamento"}},
        {{"inferno", "hades", "seol", "abismo"}},
        {{"terra", "mundo", "solo"}},
        {{"templo", "santuário", "tabernáculo"}},
        {{"igreja", "congregação", "assembleia"}},
        {{"aliança", "pacto", "promessa", "acordo", "testamento"}},
        {{"lei", "estatuto", "decreto", "mandamento", "ordenança"}},
        {{"evangelho", "boas novas", "mensagem"}},
        {{"graça", "favor", "bondade", "misericórdia"}},
        {{"glória", "esplendor", "majestade", "resplendor"}},
        {{"justo", "justiça", "reto", "virtuoso"}},
        {{"mau", "malvado", "ímpio", "pecaminoso", "corrupto", "iníquo"}},
        {{"santo", "sagrado", "santificado", "consagrado", "divino"}},
        {{"puro", "limpo", "inocente", "imaculado"}},
        {{"poder", "força", "autoridade", "domínio", "soberania"}},
        {{"sabedoria", "entendimento", "conhecimento", "discernimento", "prudência"}},
        {{"paz", "tranquilidade", "serenidade", "harmonia", "descanso"}},
        {{"esperança", "expectativa", "confiança"}},
        {{"verdade", "honestidade", "fidelidade"}},
        {{"luz", "resplendor", "iluminação"}},
        {{"trevas", "escuridão", "sombra", "noite"}},
        {{"vida", "viver", "existência"}},
        {{"eterno", "perpétuo", "imortal", "infinito"}},
        {{"anjo", "mensageiro", "serafim", "querubim"}},
        {{"demônio", "diabo", "satanás", "espírito imundo"}},
        {{"milagre", "maravilha", "sinal", "prodígio"}},
        {{"batizar", "batismo", "imersão"}},
    };
    return groups;
}

// German synonym groups
const std::vector<SynonymGroup>& germanSynonyms() {
    static const std::vector<SynonymGroup> groups = {
        {{"gott", "herr", "allmächtiger", "schöpfer"}},
        {{"jesus", "christus", "messias", "erlöser", "heiland", "retter"}},
        {{"heiliger geist", "geist", "tröster", "beistand"}},
        {{"könig", "herrscher", "souverän"}},
        {{"prophet", "seher"}},
        {{"priester", "minister"}},
        {{"apostel", "jünger", "nachfolger"}},
        {{"knecht", "diener", "sklave"}},
        {{"beten", "gebet", "fürbitte", "flehen", "anrufen"}},
        {{"anbeten", "loben", "preisen", "verherrlichen", "ehren"}},
        {{"sünde", "übertretung", "missetat", "schuld", "vergehen"}},
        {{"buße", "reue", "umkehr"}},
        {{"vergeben", "vergebung", "verzeihen"}},
        {{"retten", "rettung", "erlösen", "erlösung", "befreien"}},
        {{"segnen", "segen"}},
        {{"fluchen", "fluch", "verflucht"}},
        {{"glauben", "glaube", "vertrauen", "zuversicht"}},
        {{"liebe", "barmherzigkeit", "güte", "gnade", "erbarmen"}},
        {{"freude", "frohlocken", "jubel", "fröhlichkeit", "wonne"}},
        {{"weinen", "klagen", "trauern", "jammern"}},
        {{"heilen", "heilung", "wiederherstellen"}},
        {{"sterben", "tod", "umkommen", "vergehen"}},
        {{"töten", "erschlagen", "morden"}},
        {{"sprechen", "sagen", "verkünden", "erklären"}},
        {{"hören", "zuhören", "gehorchen"}},
        {{"sehen", "schauen", "betrachten", "erblicken"}},
        {{"himmel", "paradies", "firmament"}},
        {{"hölle", "hades", "scheol", "abgrund"}},
        {{"erde", "welt", "land", "boden"}},
        {{"tempel", "heiligtum", "stiftshütte"}},
        {{"kirche", "gemeinde", "versammlung"}},
        {{"bund", "verheißung", "versprechen", "testament"}},
        {{"gesetz", "gebot", "satzung", "ordnung"}},
        {{"evangelium", "frohe botschaft", "gute nachricht"}},
        {{"gnade", "gunst", "güte", "barmherzigkeit"}},
        {{"herrlichkeit", "glanz", "majestät", "pracht"}},
        {{"gerecht", "gerechtigkeit", "recht", "aufrecht", "tugendhaft"}},
        {{"böse", "übel", "gottlos", "sündhaft", "verdorben"}},
        {{"heilig", "geheiligt", "geweiht", "göttlich"}},
        {{"rein", "sauber", "makellos", "unschuldig"}},
        {{"macht", "kraft", "stärke", "gewalt", "herrschaft"}},
        {{"weisheit", "verstand", "erkenntnis", "einsicht", "klugheit"}},
        {{"friede", "frieden", "ruhe", "stille"}},
        {{"hoffnung", "erwartung", "zuversicht"}},
        {{"wahrheit", "ehrlichkeit", "treue"}},
        {{"licht", "glanz", "helligkeit"}},
        {{"finsternis", "dunkelheit", "schatten", "nacht"}},
        {{"leben", "dasein"}},
        {{"ewig", "unvergänglich", "unsterblich", "immerwährend"}},
        {{"engel", "bote", "seraph", "cherub"}},
        {{"dämon", "teufel", "satan", "unreiner geist"}},
        {{"wunder", "zeichen", "wundertat"}},
        {{"taufen", "taufe", "untertauchen"}},
    };
    return groups;
}

// French synonym groups
const std::vector<SynonymGroup>& frenchSynonyms() {
    static const std::vector<SynonymGroup> groups = {
        {{"dieu", "seigneur", "tout-puissant", "créateur"}},
        {{"jésus", "christ", "messie", "sauveur", "rédempteur"}},
        {{"saint-esprit", "esprit", "consolateur", "conseiller"}},
        {{"roi", "souverain", "monarque"}},
        {{"prophète", "voyant", "oracle"}},
        {{"prêtre", "ministre", "sacrificateur"}},
        {{"apôtre", "disciple", "suiveur"}},
        {{"serviteur", "esclave"}},
        {{"prier", "prière", "supplication", "intercession", "implorer"}},
        {{"adorer", "louer", "glorifier", "exalter", "honorer", "vénérer"}},
        {{"péché", "transgression", "iniquité", "méchanceté", "offense"}},
        {{"repentir", "repentance", "contrition"}},
        {{"pardonner", "pardon", "absoudre", "rémission"}},
        {{"sauver", "salut", "délivrer", "délivrance", "racheter", "rédemption"}},
        {{"bénir", "bénédiction"}},
        {{"maudire", "malédiction"}},
        {{"croire", "foi", "confiance"}},
        {{"amour", "charité", "compassion", "miséricorde", "bonté", "grâce"}},
        {{"joie", "allégresse", "réjouissance", "bonheur", "jubilation"}},
        {{"pleurer", "lamenter", "gémir", "affliger", "douleur"}},
        {{"guérir", "guérison", "restaurer"}},
        {{"mourir", "mort", "périr", "décéder", "trépas"}},
        {{"tuer", "assassiner", "frapper", "massacrer"}},
        {{"parler", "dire", "déclarer", "proclamer", "annoncer"}},
        {{"entendre", "écouter", "ouïr"}},
        {{"voir", "regarder", "observer", "contempler"}},
        {{"ciel", "paradis", "firmament"}},
        {{"enfer", "hadès", "séjour des morts", "abîme"}},
        {{"terre", "monde", "sol"}},
        {{"temple", "sanctuaire", "tabernacle"}},
        {{"église", "congrégation", "assemblée"}},
        {{"alliance", "promesse", "pacte", "testament"}},
        {{"loi", "statut", "décret", "commandement", "ordonnance"}},
        {{"évangile", "bonne nouvelle", "message"}},
        {{"grâce", "faveur", "bonté", "miséricorde"}},
        {{"gloire", "splendeur", "majesté", "éclat"}},
        {{"juste", "justice", "droit", "vertueux"}},
        {{"méchant", "mauvais", "impie", "pécheur", "corrompu"}},
        {{"saint", "sacré", "sanctifié", "consacré", "divin"}},
        {{"pur", "propre", "sans tache", "innocent", "immaculé"}},
        {{"puissance", "force", "autorité", "domination", "souveraineté"}},
        {{"sagesse", "intelligence", "connaissance", "discernement", "prudence"}},
        {{"paix", "tranquillité", "sérénité", "repos"}},
        {{"espérance", "attente", "confiance"}},
        {{"vérité", "honnêteté", "fidélité"}},
        {{"lumière", "éclat", "illumination"}},
        {{"ténèbres", "obscurité", "ombre", "nuit"}},
        {{"vie", "vivre", "existence"}},
        {{"éternel", "perpétuel", "immortel", "infini"}},
        {{"ange", "messager", "séraphin", "chérubin"}},
        {{"démon", "diable", "satan", "esprit impur"}},
        {{"miracle", "prodige", "signe", "merveille"}},
        {{"baptiser", "baptême", "immersion"}},
    };
    return groups;
}

using SynonymIndex = std::unordered_map<std::string, std::vector<std::string>>;

SynonymIndex buildIndex(const std::vector<SynonymGroup>& groups) {
    SynonymIndex index;
    for (const auto& group : groups) {
        // Collect all lowered forms for the group
        std::vector<std::string> allForms;
        allForms.reserve(group.words.size());
        for (const auto& word : group.words) {
            allForms.push_back(toLower(word));
        }

        for (size_t i = 0; i < allForms.size(); ++i) {
            const std::string& key = allForms[i];

            // Add synonyms under the exact (lowered) key
            for (size_t j = 0; j < allForms.size(); ++j) {
                if (i != j) {
                    index[key].push_back(allForms[j]);
                }
            }

            // Also add under the accent-stripped key so that "senor" finds
            // synonyms keyed under "señor", etc.
            std::string stripped = toLower(doStripDiacritics(key));
            if (stripped != key) {
                for (size_t j = 0; j < allForms.size(); ++j) {
                    // Include ALL group members (including the accented original)
                    // under the stripped key.
                    index[stripped].push_back(allForms[j]);
                }
            }
        }
    }
    // Deduplicate
    for (auto& [key, synonyms] : index) {
        std::sort(synonyms.begin(), synonyms.end());
        synonyms.erase(std::unique(synonyms.begin(), synonyms.end()), synonyms.end());
    }
    return index;
}

const SynonymIndex& synonymIndex(const std::string& lang) {
    // Normalize language code to base form (e.g. "en-US" -> "en")
    std::string base = toLower(lang);
    if (base.size() > 2) {
        size_t sep = base.find_first_of("-_");
        if (sep != std::string::npos) base = base.substr(0, sep);
    }

    static const SynonymIndex enIndex = buildIndex(englishSynonyms());
    static const SynonymIndex esIndex = buildIndex(spanishSynonyms());
    static const SynonymIndex ptIndex = buildIndex(portugueseSynonyms());
    static const SynonymIndex deIndex = buildIndex(germanSynonyms());
    static const SynonymIndex frIndex = buildIndex(frenchSynonyms());
    static const SynonymIndex empty;

    if (base == "en") return enIndex;
    if (base == "es") return esIndex;
    if (base == "pt") return ptIndex;
    if (base == "de") return deIndex;
    if (base == "fr") return frIndex;
    return empty;
}

// ─── Phonetic matching (Metaphone-like) ──────────────────────────────

// Simplified Metaphone for English words. Generates a phonetic key that
// groups similar-sounding words together, helping catch misspellings
// that preserve pronunciation.
std::string computeMetaphone(const std::string& word) {
    if (word.empty()) return "";

    std::string input = toLower(word);
    // Strip non-alpha
    std::string clean;
    for (char c : input) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            clean.push_back(c);
        }
    }
    if (clean.empty()) return "";

    std::string result;
    result.reserve(8);

    // Drop initial silent letters
    if (clean.size() >= 2) {
        std::string prefix = clean.substr(0, 2);
        if (prefix == "ae" || prefix == "gn" || prefix == "kn" ||
            prefix == "pn" || prefix == "wr") {
            clean = clean.substr(1);
        }
    }

    size_t len = clean.size();
    for (size_t i = 0; i < len && result.size() < 6; ++i) {
        char c = clean[i];
        char prev = (i > 0) ? clean[i - 1] : 0;
        char next = (i + 1 < len) ? clean[i + 1] : 0;

        // Skip duplicate adjacent letters
        if (c == prev && c != 'c') continue;

        switch (c) {
        case 'a': case 'e': case 'i': case 'o': case 'u':
            if (i == 0) result.push_back('A');
            break;
        case 'b':
            if (prev != 'm') result.push_back('B');
            break;
        case 'c':
            if (next == 'e' || next == 'i' || next == 'y') {
                result.push_back('S');
            } else {
                result.push_back('K');
            }
            break;
        case 'd':
            if (next == 'g' && (i + 2 < len) &&
                (clean[i + 2] == 'e' || clean[i + 2] == 'i' || clean[i + 2] == 'y')) {
                result.push_back('J');
            } else {
                result.push_back('T');
            }
            break;
        case 'f':
            result.push_back('F');
            break;
        case 'g':
            if (i + 1 < len && next == 'h' && i + 2 < len &&
                !(clean[i + 2] == 'a' || clean[i + 2] == 'e' || clean[i + 2] == 'i' ||
                  clean[i + 2] == 'o' || clean[i + 2] == 'u')) {
                break; // silent gh
            }
            if (i > 0 && (next == 'e' || next == 'i' || next == 'y')) {
                result.push_back('J');
            } else if (next != 'h' || i == 0) {
                result.push_back('K');
            }
            break;
        case 'h':
            if ((next == 'a' || next == 'e' || next == 'i' || next == 'o' || next == 'u') &&
                prev != 'a' && prev != 'e' && prev != 'i' && prev != 'o' && prev != 'u') {
                result.push_back('H');
            }
            break;
        case 'j':
            result.push_back('J');
            break;
        case 'k':
            if (prev != 'c') result.push_back('K');
            break;
        case 'l':
            result.push_back('L');
            break;
        case 'm':
            result.push_back('M');
            break;
        case 'n':
            result.push_back('N');
            break;
        case 'p':
            if (next == 'h') {
                result.push_back('F');
                ++i;
            } else {
                result.push_back('P');
            }
            break;
        case 'q':
            result.push_back('K');
            break;
        case 'r':
            result.push_back('R');
            break;
        case 's':
            if (next == 'h' || (next == 'i' && i + 2 < len &&
                (clean[i + 2] == 'o' || clean[i + 2] == 'a'))) {
                result.push_back('X');
                ++i;
            } else {
                result.push_back('S');
            }
            break;
        case 't':
            if (next == 'h') {
                result.push_back('0'); // theta
                ++i;
            } else if (next == 'i' && i + 2 < len &&
                       (clean[i + 2] == 'o' || clean[i + 2] == 'a')) {
                result.push_back('X');
            } else {
                result.push_back('T');
            }
            break;
        case 'v':
            result.push_back('F');
            break;
        case 'w':
        case 'y':
            if (next == 'a' || next == 'e' || next == 'i' || next == 'o' || next == 'u') {
                result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
            break;
        case 'x':
            result.push_back('K');
            result.push_back('S');
            break;
        case 'z':
            result.push_back('S');
            break;
        default:
            break;
        }
    }

    return result;
}

// ─── Tokenization ────────────────────────────────────────────────────

std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> words;
    std::istringstream ss(text);
    std::string word;
    while (ss >> word) {
        // Strip leading/trailing punctuation
        size_t start = 0, end = word.size();
        while (start < end && !std::isalnum(static_cast<unsigned char>(word[start])) &&
               static_cast<unsigned char>(word[start]) < 0x80) ++start;
        while (end > start && !std::isalnum(static_cast<unsigned char>(word[end - 1])) &&
               static_cast<unsigned char>(word[end - 1]) < 0x80) --end;
        if (start < end) {
            words.push_back(word.substr(start, end - start));
        }
    }
    return words;
}

std::string quoteFtsToken(const std::string& token) {
    std::string escaped;
    escaped.reserve(token.size() + 4);
    for (char c : token) {
        if (c == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(c);
        }
    }
    return "\"" + escaped + "\"";
}

} // namespace

// ─── Public API ──────────────────────────────────────────────────────

int editDistance(const std::string& a, const std::string& b) {
    std::string la = toLower(a);
    std::string lb = toLower(b);

    const size_t m = la.size();
    const size_t n = lb.size();
    if (m == 0) return static_cast<int>(n);
    if (n == 0) return static_cast<int>(m);

    // Use single-row optimization for memory efficiency
    std::vector<int> prev(n + 1);
    std::vector<int> curr(n + 1);

    for (size_t j = 0; j <= n; ++j) prev[j] = static_cast<int>(j);

    for (size_t i = 1; i <= m; ++i) {
        curr[0] = static_cast<int>(i);
        for (size_t j = 1; j <= n; ++j) {
            int cost = (la[i - 1] == lb[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1,           // deletion
                                curr[j - 1] + 1,       // insertion
                                prev[j - 1] + cost});   // substitution
        }
        std::swap(prev, curr);
    }

    return prev[n];
}

int damerauLevenshteinDistance(const std::string& a, const std::string& b) {
    std::string la = toLower(a);
    std::string lb = toLower(b);

    const size_t m = la.size();
    const size_t n = lb.size();
    if (m == 0) return static_cast<int>(n);
    if (n == 0) return static_cast<int>(m);

    // Full matrix needed for transposition check
    std::vector<std::vector<int>> d(m + 1, std::vector<int>(n + 1));

    for (size_t i = 0; i <= m; ++i) d[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= n; ++j) d[0][j] = static_cast<int>(j);

    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            int cost = (la[i - 1] == lb[j - 1]) ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1,
                                d[i][j - 1] + 1,
                                d[i - 1][j - 1] + cost});

            // Transposition
            if (i > 1 && j > 1 &&
                la[i - 1] == lb[j - 2] && la[i - 2] == lb[j - 1]) {
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + cost);
            }
        }
    }

    return d[m][n];
}

double fuzzyScore(const std::string& query, const std::string& candidate) {
    std::string q = toLower(query);
    std::string c = toLower(candidate);

    if (q.empty() || c.empty()) return 0.0;

    // Exact match
    if (q == c) return 1.0;

    // Prefix match bonus
    double prefixBonus = 0.0;
    size_t commonPrefix = 0;
    while (commonPrefix < q.size() && commonPrefix < c.size() &&
           q[commonPrefix] == c[commonPrefix]) {
        ++commonPrefix;
    }
    if (commonPrefix > 0) {
        prefixBonus = 0.15 * (static_cast<double>(commonPrefix) /
                              static_cast<double>(std::max(q.size(), c.size())));
    }

    // Substring containment
    if (c.find(q) != std::string::npos) {
        // Query is contained in candidate
        double lenRatio = static_cast<double>(q.size()) / static_cast<double>(c.size());
        return std::min(0.95, 0.7 + 0.25 * lenRatio + prefixBonus);
    }
    if (q.find(c) != std::string::npos) {
        // Candidate is contained in query
        double lenRatio = static_cast<double>(c.size()) / static_cast<double>(q.size());
        return std::min(0.85, 0.5 + 0.3 * lenRatio + prefixBonus);
    }

    // Edit distance scoring
    int dist = damerauLevenshteinDistance(q, c);
    int maxLen = static_cast<int>(std::max(q.size(), c.size()));

    // Adaptive threshold: allow more edits for longer words
    int threshold;
    if (maxLen <= 3) threshold = 1;
    else if (maxLen <= 5) threshold = 1;
    else if (maxLen <= 8) threshold = 2;
    else threshold = 3;

    if (dist > threshold) return 0.0;

    double distScore = 1.0 - (static_cast<double>(dist) / static_cast<double>(maxLen));
    return std::min(0.9, distScore * 0.8 + prefixBonus);
}

std::vector<std::string> expandSynonyms(const std::string& word,
                                          const std::string& language) {
    std::vector<std::string> result;
    result.push_back(word);

    std::string lower = toLower(word);
    const auto& index = synonymIndex(language);
    std::unordered_set<std::string> seen;
    seen.insert(lower);

    // Try exact (lowered) key first
    auto it = index.find(lower);
    if (it != index.end()) {
        for (const auto& syn : it->second) {
            if (seen.insert(syn).second) {
                result.push_back(syn);
            }
        }
    }

    // Also try the accent-stripped form so that e.g. "senor" finds synonyms
    // of "señor", and "gracia" finds synonyms of "grâce".
    std::string stripped = toLower(doStripDiacritics(lower));
    if (stripped != lower) {
        auto it2 = index.find(stripped);
        if (it2 != index.end()) {
            for (const auto& syn : it2->second) {
                if (seen.insert(syn).second) {
                    result.push_back(syn);
                }
            }
        }
    }

    return result;
}

bool hasSynonymDatabase(const std::string& language) {
    return !synonymIndex(language).empty();
}

std::vector<std::string> supportedSynonymLanguages() {
    return {"en", "es", "pt", "de", "fr"};
}

std::string stripDiacritics(const std::string& text) {
    return doStripDiacritics(text);
}

std::string metaphoneKey(const std::string& word) {
    return computeMetaphone(word);
}

std::vector<std::string> generateTypoVariants(const std::string& word) {
    std::vector<std::string> variants;
    std::string lower = toLower(word);
    if (lower.size() < 3) return variants;

    std::unordered_set<std::string> seen;
    seen.insert(lower);

    auto addIfNew = [&](const std::string& v) {
        if (v.size() >= 2 && seen.insert(v).second) {
            variants.push_back(v);
        }
    };

    // Limit variant generation for very long words
    size_t len = lower.size();
    if (len > 12) return variants;

    // Single character deletions
    for (size_t i = 0; i < len; ++i) {
        addIfNew(lower.substr(0, i) + lower.substr(i + 1));
    }

    // Adjacent transpositions
    for (size_t i = 0; i + 1 < len; ++i) {
        std::string t = lower;
        std::swap(t[i], t[i + 1]);
        addIfNew(t);
    }

    // Common character substitutions.
    // Vowels map to ALL other vowels so that "haly"→"holy", "hely"→"holy", etc.
    // Consonants map to phonetically similar or keyboard-adjacent letters.
    static const std::unordered_map<char, std::string> confusions = {
        {'a', "eiou"},  {'e', "aiou"},  {'i', "aeou"},  {'o', "aeiu"},  {'u', "aeio"},
        {'b', "vpd"},   {'c', "ksg"},   {'d', "tbg"},   {'f', "vph"},   {'g', "jkcd"},
        {'h', ""},      {'j', "gi"},    {'k', "cg"},    {'l', "r"},     {'m', "n"},
        {'n', "m"},     {'p', "b"},     {'q', "k"},     {'r', "l"},     {'s', "zc"},
        {'t', "d"},     {'v', "bfw"},   {'w', "v"},     {'x', "ks"},    {'y', "ie"},
        {'z', "s"},
    };

    for (size_t i = 0; i < len; ++i) {
        auto it = confusions.find(lower[i]);
        if (it != confusions.end()) {
            for (char replacement : it->second) {
                std::string v = lower;
                v[i] = replacement;
                addIfNew(v);
            }
        }
    }

    // Double-letter reduction: "holly"→"holy", "holyy"→"holy", etc.
    // Remove one instance of any doubled letter.
    for (size_t i = 0; i + 1 < len; ++i) {
        if (lower[i] == lower[i + 1]) {
            addIfNew(lower.substr(0, i) + lower.substr(i + 1));
        }
    }

    // Double-letter insertion: "holy"→"holly", "holy"→"hooly", etc.
    // Insert a copy of each character next to itself.
    if (len <= 8) {
        for (size_t i = 0; i < len; ++i) {
            addIfNew(lower.substr(0, i + 1) + lower[i] + lower.substr(i + 1));
        }
    }

    return variants;
}

std::string buildSmartFtsQuery(const std::string& query,
                                const std::string& language) {
    std::vector<std::string> words = splitWords(query);
    if (words.empty()) return "";

    // For each query word, build a group of alternatives: synonyms + the
    // original + fuzzy/phonetic variants. Within each group, terms are OR'd.
    // Groups are AND'd together so all query concepts must appear.
    std::ostringstream fts;
    fts << "{title content}:(";

    for (size_t w = 0; w < words.size(); ++w) {
        if (w > 0) fts << " AND ";

        std::string word = toLower(words[w]);
        // Also try the accent-stripped form of the query word itself
        std::string strippedWord = toLower(doStripDiacritics(word));

        std::unordered_set<std::string> alternatives;
        alternatives.insert(word);
        if (strippedWord != word) alternatives.insert(strippedWord);

        // Add synonyms (tries both accented and stripped forms internally)
        auto syns = expandSynonyms(word, language);
        for (const auto& s : syns) {
            alternatives.insert(toLower(s));
        }
        if (strippedWord != word) {
            auto syns2 = expandSynonyms(strippedWord, language);
            for (const auto& s : syns2) {
                alternatives.insert(toLower(s));
            }
        }

        // Add typo/misspelling variants so FTS5 can find close matches.
        // This is the key mechanism for catching "holly"→"holy", "haly"→"holy".
        auto typoVars = generateTypoVariants(word);
        for (const auto& tv : typoVars) {
            alternatives.insert(tv);
        }
        if (strippedWord != word) {
            auto typoVars2 = generateTypoVariants(strippedWord);
            for (const auto& tv : typoVars2) {
                alternatives.insert(tv);
            }
        }

        // Build the FTS OR group
        fts << "(";
        bool first = true;

        auto isSafeFtsToken = [](const std::string& tok) {
            if (tok.empty()) return false;
            for (char ch : tok) {
                unsigned char uc = static_cast<unsigned char>(ch);
                if (!std::isalnum(uc) && ch != '\'' && ch != '-' && uc < 0x80) {
                    // Allow spaces (for multi-word synonyms, which get quoted)
                    if (ch != ' ') return false;
                }
            }
            return true;
        };

        for (const auto& alt : alternatives) {
            if (!isSafeFtsToken(alt)) continue;
            if (!first) fts << " OR ";
            first = false;
            fts << quoteFtsToken(alt);
        }

        // Add prefix match for original word (catches morphological variants).
        // FTS5 prefix syntax: token* (unquoted token with trailing asterisk).
        if (word.size() >= 4) {
            size_t stemLen = word.size() - 1;
            if (stemLen >= 3) {
                std::string stem = word.substr(0, stemLen);
                if (alternatives.find(stem) == alternatives.end()) {
                    bool safe = true;
                    for (char ch : stem) {
                        if (!std::isalnum(static_cast<unsigned char>(ch))) {
                            safe = false;
                            break;
                        }
                    }
                    if (safe) {
                        fts << " OR " << stem << "*";
                    }
                }
            }
        }

        fts << ")";
    }

    fts << ")";
    return fts.str();
}

std::vector<ScoredMatch> scoreSmartResults(
    const std::vector<std::string>& queryTerms,
    const std::vector<std::string>& resultTexts,
    const std::string& language) {

    std::vector<ScoredMatch> scored;
    scored.reserve(resultTexts.size());

    // Pre-compute lowered query terms, accent-stripped forms, and their synonyms
    std::vector<std::string> lowerTerms;
    std::vector<std::string> strippedTerms;
    std::vector<std::unordered_set<std::string>> termSynSets;
    for (const auto& term : queryTerms) {
        std::string lt = toLower(term);
        lowerTerms.push_back(lt);
        std::string st = toLower(doStripDiacritics(lt));
        strippedTerms.push_back(st);

        auto syns = expandSynonyms(lt, language);
        std::unordered_set<std::string> synSet;
        for (const auto& s : syns) {
            synSet.insert(toLower(s));
            // Also add accent-stripped form of each synonym
            std::string ss = toLower(doStripDiacritics(s));
            if (ss != s) synSet.insert(ss);
        }
        termSynSets.push_back(std::move(synSet));
    }

    // Pre-compute metaphone keys for query terms (use stripped forms)
    std::vector<std::string> termPhonetic;
    for (const auto& st : strippedTerms) {
        termPhonetic.push_back(computeMetaphone(st));
    }

    for (size_t i = 0; i < resultTexts.size(); ++i) {
        ScoredMatch match;
        match.rowIndex = static_cast<int>(i);

        std::string lowerText = toLower(resultTexts[i]);
        std::string strippedText = toLower(doStripDiacritics(lowerText));
        std::vector<std::string> textWords = splitWords(lowerText);
        // Also split the stripped text for accent-insensitive word matching
        std::vector<std::string> strippedTextWords = splitWords(strippedText);

        // Merge word lists (deduplicated) for matching
        std::unordered_set<std::string> allTextWords;
        for (const auto& tw : textWords) allTextWords.insert(tw);
        for (const auto& tw : strippedTextWords) allTextWords.insert(tw);

        double totalTermScore = 0.0;
        int exactMatches = 0;
        int synonymMatches = 0;
        int fuzzyMatches = 0;
        int termCount = static_cast<int>(lowerTerms.size());

        for (int t = 0; t < termCount; ++t) {
            double bestScore = 0.0;
            bool isExact = false;
            bool isSynonym = false;

            for (const auto& tw : allTextWords) {
                // Exact match (also matches accent-stripped forms)
                if (tw == lowerTerms[t] || tw == strippedTerms[t]) {
                    bestScore = 1.0;
                    isExact = true;
                    break;
                }

                // Synonym match
                if (termSynSets[t].count(tw) > 0) {
                    double score = 0.85;
                    if (score > bestScore) {
                        bestScore = score;
                        isSynonym = true;
                    }
                    continue;
                }

                // Phonetic match
                if (!termPhonetic[t].empty()) {
                    std::string twPhonetic = computeMetaphone(tw);
                    if (!twPhonetic.empty() && twPhonetic == termPhonetic[t]) {
                        double score = 0.75;
                        if (score > bestScore) {
                            bestScore = score;
                        }
                        continue;
                    }
                }

                // Fuzzy match (only for reasonable length words)
                if (tw.size() >= 3 && strippedTerms[t].size() >= 3) {
                    // Try fuzzy against both accented and stripped query term
                    double fs = std::max(fuzzyScore(lowerTerms[t], tw),
                                         fuzzyScore(strippedTerms[t], tw));
                    if (fs > bestScore) {
                        bestScore = fs;
                    }
                }
            }

            totalTermScore += bestScore;
            if (isExact) ++exactMatches;
            else if (isSynonym) ++synonymMatches;
            else if (bestScore > 0.0) ++fuzzyMatches;
        }

        if (termCount > 0) {
            match.fuzzyScore = totalTermScore / termCount;
        }

        match.exactMatch = (exactMatches > 0);
        match.synonymMatch = (synonymMatches > 0);

        // Combined score: heavily weight exact matches, then synonyms, then fuzzy
        double exactBonus = (exactMatches > 0) ? 0.3 * (static_cast<double>(exactMatches) / termCount) : 0.0;
        match.combinedScore = match.fuzzyScore + exactBonus;

        // Only include results with reasonable match quality
        if (match.combinedScore > 0.0) {
            scored.push_back(match);
        }
    }

    // Sort by combined score descending
    std::sort(scored.begin(), scored.end(),
              [](const ScoredMatch& a, const ScoredMatch& b) {
                  return a.combinedScore > b.combinedScore;
              });

    return scored;
}

} // namespace smart_search
} // namespace verdad
