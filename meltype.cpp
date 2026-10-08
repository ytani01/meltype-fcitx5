// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yukishiro
// Copyright (C) 2026 Yoichi Tanibayashi
//
// Meltype の fcitx5 版 (入力メソッドのアドオン)。IBus 版 (linux/ibus-engine-meltype) を書き直したもの。
// キーを受け取って Meltype の本体 (libMeltypeNative.so) に渡し、返ってきた結果 (確定する文字・変換中の文字・候補) を
// 入力欄に出す。漢字変換は Mozc の変換ヘルパー (meltype_mozc_helper) を本体が呼ぶ。
//
// 本体の場所は環境変数 MELTYPE_DIR で渡す。配布 zip を展開したディレクトリ
// (libMeltypeNative.so と mozc/meltype_mozc_helper がある所) を指す。無ければビルド時の MELTYPE_DEFAULT_DIR
// (既定は /opt/meltype、install.sh が本体をインストールする所。パッケージは /usr/lib/meltype-fcitx5)。
//
// 変換中のキーの割り当ては ~/.config/meltype-fcitx5/keymap.conf (1 行 1 組で「キー = 置き換え先のキー」) に置く。
// 置き換え先を空にした行 (Down =) のキーは、本体に渡さずアプリに渡す。
// アドオン設定 (fcitx5-configtool) も同じファイルを読み書きする。ファイルが無ければ何も置き換えない。
#include <dlfcn.h>
#include <json-c/json.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx-utils/utf8.h>
#include <fcitx-config/configuration.h>
#include <fcitx-config/option.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>

namespace {

using namespace fcitx;

// ---- 本体 (libMeltypeNative.so) ----

struct Native {
    int (*initMozc)(const char *, const char *) = nullptr;
    void *(*create)() = nullptr;
    void (*destroy)(void *) = nullptr;
    char *(*handleKey)(void *, int, int, int, const char *, const char *) = nullptr;
    char *(*commit)(void *) = nullptr;
    char *(*selectCandidate)(void *, int) = nullptr;
    void (*setDirect)(void *, int) = nullptr;
    void (*setCanDelete)(void *, int) = nullptr; // 1.1.0 から。古い本体には無い
    void (*free)(char *) = nullptr;
    std::filesystem::path dataDirectory; // 学習の記録の置き場所 (meltype_data_directory)。分からなければ空

    // 本体を読む。読めなければ create を nullptr にする (キーはすべてアプリに渡す)。
    // .NET のランタイムは閉じられないので dlclose はしない。
    void load() {
        const char *dir = std::getenv("MELTYPE_DIR");
        const std::string base = dir && *dir ? dir : MELTYPE_DEFAULT_DIR;
        void *handle = dlopen((base + "/libMeltypeNative.so").c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            FCITX_ERROR() << "Meltype: 本体を読めません: " << dlerror();
            return;
        }
        bool ok = true;
        auto symbol = [&](auto &target, const char *name) {
            target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(dlsym(handle, name));
            if (!target) {
                FCITX_ERROR() << "Meltype: 本体に " << name << " がありません";
                ok = false;
            }
        };
        symbol(initMozc, "meltype_init_mozc");
        symbol(create, "meltype_create");
        symbol(destroy, "meltype_destroy");
        symbol(handleKey, "meltype_handle_key");
        symbol(commit, "meltype_commit");
        symbol(selectCandidate, "meltype_select_candidate");
        symbol(setDirect, "meltype_set_direct");
        symbol(free, "meltype_free");
        if (!ok) {
            create = nullptr;
            return;
        }
        setCanDelete = reinterpret_cast<decltype(setCanDelete)>(dlsym(handle, "meltype_set_can_delete"));
        if (auto directory = reinterpret_cast<char *(*)()>(dlsym(handle, "meltype_data_directory"))) {
            if (char *path = directory()) {
                dataDirectory = path;
                free(path);
            }
        }
        if (!initMozc((base + "/mozc/meltype_mozc_helper").c_str(), nullptr)) {
            FCITX_ERROR() << "Meltype: Mozc を使えません (漢字変換はひらがなのままになります)";
        }
    }

    // 本体が返した JSON (SessionResult) を読んで解放する。NULL なら nullptr (キーはアプリに渡す)。
    json_object *result(char *pointer) const {
        if (!pointer) {
            return nullptr;
        }
        json_object *object = json_tokener_parse(pointer);
        free(pointer);
        return object;
    }
};

// JSON の値を取り出す (無い・null なら既定の値)
json_object *member(json_object *object, const char *key) {
    json_object *value = nullptr;
    return object && json_object_object_get_ex(object, key, &value) ? value : nullptr;
}
std::string stringOf(json_object *object, const char *key) {
    json_object *value = member(object, key);
    return value && json_object_is_type(value, json_type_string) ? json_object_get_string(value) : "";
}
int intOf(json_object *object, const char *key, int fallback) {
    json_object *value = member(object, key);
    return value ? json_object_get_int(value) : fallback;
}
bool boolOf(json_object *object, const char *key) {
    json_object *value = member(object, key);
    return value && json_object_get_boolean(value);
}
std::vector<std::string> stringsOf(json_object *object, const char *key) {
    std::vector<std::string> list;
    json_object *value = member(object, key);
    if (value && json_object_is_type(value, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(value); i++) {
            const char *text = json_object_get_string(json_object_array_get_idx(value, i));
            list.emplace_back(text ? text : "");
        }
    }
    return list;
}

// ---- キー: fcitx5 のキー (X の keysym) → Meltype の本体が使う Windows の仮想キーコード ----

// 文字を入力するキーで、特別な扱いのないもの (記号など)。本体では文字だけを見る。
constexpr int OTHER_CHARACTER_KEY = 0x07;

int specialKey(KeySym sym) {
    switch (sym) {
    case FcitxKey_Return:
    case FcitxKey_KP_Enter: return 0x0D;
    case FcitxKey_Tab: return 0x09;
    case FcitxKey_space: return 0x20;
    case FcitxKey_BackSpace: return 0x08;
    case FcitxKey_Delete: return 0x2E;
    case FcitxKey_Escape: return 0x1B;
    case FcitxKey_Left: return 0x25;
    case FcitxKey_Up: return 0x26;
    case FcitxKey_Right: return 0x27;
    case FcitxKey_Down: return 0x28;
    case FcitxKey_Home: return 0x24;
    case FcitxKey_End: return 0x23;
    case FcitxKey_Page_Up: return 0x21;
    case FcitxKey_Page_Down: return 0x22;
    case FcitxKey_F6: return 0x75;
    case FcitxKey_F7: return 0x76;
    case FcitxKey_F8: return 0x77;
    case FcitxKey_F9: return 0x78;
    case FcitxKey_F10: return 0x79;
    default: return 0;
    }
}

int symbolKey(uint32_t character) {
    switch (character) {
    case ',': return 0xBC;
    case '.': return 0xBE;
    case '-': return 0xBD;
    case '/': return 0xBF;
    case '[': return 0xDB;
    case ']': return 0xDD;
    default: return OTHER_CHARACTER_KEY;
    }
}

// (仮想キーコード, 入力する文字) を返す。Meltype が扱わないキーなら仮想キーコードが 0。
std::pair<int, int> virtualKey(KeySym sym) {
    if (int vk = specialKey(sym)) {
        return {vk, sym == FcitxKey_space ? 0x20 : 0};
    }
    const uint32_t character = Key::keySymToUnicode(sym);
    if (character < 0x20) {
        return {0, 0};
    }
    if (character >= 'a' && character <= 'z') {
        return {static_cast<int>(character) - 0x20, static_cast<int>(character)};
    }
    if ((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')) {
        return {static_cast<int>(character), static_cast<int>(character)};
    }
    return {symbolKey(character), static_cast<int>(character)};
}

bool isModeKey(KeySym sym) {
    switch (sym) {
    case FcitxKey_Zenkaku_Hankaku:
    case FcitxKey_Hankaku:
    case FcitxKey_Zenkaku:
    case FcitxKey_Eisu_toggle:
    case FcitxKey_Hiragana_Katakana:
    case FcitxKey_Muhenkan:
    case FcitxKey_Henkan: return true;
    default: return false;
    }
}

// ---- 変換中のキーの割り当て ----

// 割り当ての 1 組。左が押すキー、右が置き換え先のキー。置き換え先が空 (Key()) なら、本体に渡さずアプリに渡す。
using KeyMap = std::vector<std::pair<Key, Key>>;

// 置き換え先のキーのうち、変換中に本体が意味を持たせているもの (~/work/Meltype の CompositionController.cs)。
// アドオン設定の画面には、これを 1 つずつ「意味（キー）」の欄にして、押すキーの一覧を並べる。
// 最後の欄 (置き換え先が空) は、本体に渡さずアプリに渡すキー。
struct Action {
    const char *name;    // 設定の項目名
    const char *key;     // 置き換え先のキー
    const char *meaning; // 変換中に本体がすること
};
constexpr Action ACTIONS[] = {
    {"Space", "space", "変換・次の候補"},
    {"ShiftSpace", "Shift+space", "ローマ字として変換・次の候補"},
    {"Down", "Down", "次の候補"},
    {"Up", "Up", "前の候補"},
    {"Right", "Right", "次の文節"},
    {"Left", "Left", "前の文節"},
    {"ShiftRight", "Shift+Right", "文節を伸ばす"},
    {"ShiftLeft", "Shift+Left", "文節を縮める"},
    {"Return", "Return", "確定"},
    {"BackSpace", "BackSpace", "1 文字消す・変換の取り消し"},
    {"Escape", "Escape", "取り消し"},
    {"Tab", "Tab", "もしかして（書き間違いを直す）"},
    {"F6", "F6", "ひらがな"},
    {"F7", "F7", "カタカナ"},
    {"F9", "F9", "全角英数"},
    {"F10", "F10", "半角英数"},
    {"PassThrough", "", "本体に渡さずアプリに渡す"},
};

// アドオン設定。置き換え先ごとに、押すキーの一覧を持つ。中身は keymap.conf から作り、保存すると keymap.conf に書く。
class MeltypeConfig : public Configuration {
public:
    MeltypeConfig() {
        for (const Action &action : ACTIONS) {
            keys.push_back(std::make_unique<KeyListOption>(
                this, action.name, *action.key ? std::string(action.meaning) + "（" + action.key + "）" : action.meaning,
                KeyList(),
                KeyListConstrain(KeyConstrainFlag::AllowModifierLess)));
        }
    }
    const char *typeName() const override { return "MeltypeConfig"; }

    std::vector<std::unique_ptr<KeyListOption>> keys; // ACTIONS と同じ順
};

std::filesystem::path keyMapPath() {
    const char *config = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    const std::filesystem::path base =
        config && *config ? std::filesystem::path(config) : std::filesystem::path(home ? home : "") / ".config";
    return base / "meltype-fcitx5" / "keymap.conf";
}

// 本体がセッションごとに読んで持つ学習の記録のうち、いちばん新しい更新日時 (無ければ最小の値)。
// 本体は作ったときに 1 回だけ読み、保存では自分の持っている分だけで書き直すので、別の入力欄が覚えた分を消してしまう。
std::filesystem::file_time_type learnedTime(const std::filesystem::path &base) {
    auto latest = std::filesystem::file_time_type::min();
    for (const char *name : {"conversions.json", "languages.json", "translations.json"}) {
        std::error_code error;
        const auto time = std::filesystem::last_write_time(base / name, error);
        if (!error) {
            latest = std::max(latest, time);
        }
    }
    return latest;
}

// Shift と小文字の英字の組 (Shift+a) を、実際に打ったときと同じ大文字 (Shift+A) にする。
// そのまま正規化すると Shift だけが消えて a になり、Shift 無しの a に一致してしまう。
Key shiftedLetter(const Key &key) {
    const KeySym sym = key.sym();
    if (key.states().test(KeyState::Shift) && sym >= FcitxKey_a && sym <= FcitxKey_z) {
        return Key(static_cast<KeySym>(sym - FcitxKey_a + FcitxKey_A), key.states());
    }
    return key;
}

// 置き換え先が ACTIONS の key と同じキーか (Control+Down と Ctrl+Down のような書き方の違いは正規化して比べる)
bool sameKey(const Key &a, const Key &b) { return a.normalize() == b.normalize(); }

// 読めない行は飛ばしてログに出す。# で始まる行と空行は無視する。ファイルが無ければ空。
// 置き換え先が空の行 (Down =) は、置き換え先を Key() にする。
KeyMap readKeyMap(const std::filesystem::path &path) {
    KeyMap entries;
    std::ifstream file(path);
    if (!file && std::filesystem::exists(path)) {
        FCITX_WARN() << "Meltype: " << path.string() << " を開けません";
    }
    std::string line;
    for (int number = 1; std::getline(file, line); number++) {
        const std::string_view text = stringutils::trimView(line);
        if (text.empty() || text.front() == '#') {
            continue;
        }
        const size_t equal = text.find('=');
        if (equal != std::string_view::npos) {
            const Key from{std::string(stringutils::trimView(text.substr(0, equal)))};
            const std::string_view right = stringutils::trimView(text.substr(equal + 1));
            const Key to = right.empty() ? Key() : Key(std::string(right));
            if (from.isValid() && !from.isModifier() && (right.empty() || (to.isValid() && !to.isModifier()))) {
                entries.emplace_back(from, to);
                continue;
            }
        }
        FCITX_WARN() << "Meltype: " << path.string() << ":" << number << " を読めません: " << line;
    }
    return entries;
}

// 割り当ての行だけで書き直す (コメントと空行は残らない)。途中で失敗しても元のファイルを壊さないよう、別名で書いてから置き換える。
// シンボリックリンクならリンク先を書き直す (dotfiles で管理していてもリンクが外れない)。
void writeKeyMap(const std::filesystem::path &link, const KeyMap &entries) {
    std::error_code error;
    std::filesystem::path path = std::filesystem::weakly_canonical(link, error);
    if (error) {
        path = link;
    }
    std::filesystem::create_directories(path.parent_path(), error);
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream file(temporary);
    for (const auto &[from, to] : entries) {
        file << from.toString() << " =" << (to.isValid() ? " " + to.toString() : "") << "\n";
    }
    file.close();
    if (!file) {
        FCITX_ERROR() << "Meltype: " << temporary.string() << " に書けません";
        std::filesystem::remove(temporary, error);
        return;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        FCITX_ERROR() << "Meltype: " << path.string() << " に書けません: " << error.message();
    }
}

// ---- 入力欄ごとの状態 ----

class MeltypeState;

// 候補ウィンドウの候補。選ぶと本体に何番目かを渡す。
class MeltypeCandidate : public CandidateWord {
public:
    MeltypeCandidate(MeltypeState *state, int index, const std::string &text)
        : CandidateWord(Text(text)), state_(state), index_(index) {}
    void select(InputContext *) const override;

private:
    MeltypeState *state_;
    int index_;
};

class MeltypeState : public InputContextProperty {
public:
    MeltypeState(const Native &native, const KeyMap &keyMap, Instance *instance, InputContext &ic)
        : native_(native), keyMap_(keyMap), instance_(instance), ic_(ic),
          session_(nullptr) {}
    ~MeltypeState() override {
        if (session_) {
            native_.destroy(session_);
        }
    }

    // 本体のセッションは、Meltype を使う入力欄にだけ作る (IBus 版と同じ)。
    // 作れなかった入力欄では作り直さない (キーはアプリに渡す)。
    bool ensureSession() {
        if (!session_ && !createFailed_ && native_.create) {
            session_ = native_.create();
            if (!session_) {
                createFailed_ = true;
                FCITX_ERROR() << "Meltype: 入力欄のセッションを作れません (この入力欄ではキーをアプリに渡します)";
            }
            noteLearned();
        }
        return session_;
    }

    // 別の入力欄が学習を保存していたら、セッションを作り直して読み直させる。
    // フォーカスを得たときと、未確定の文字が無いときのキーで確かめる (fcitx5 のフォーカスは wayland と X11 で別々に持つので、
    // フォーカスを得たときだけでは足りない)。自分で保存した分は noteLearned で読んだことにする。
    // ponytail: 未確定の文字があるあいだに別の入力欄が保存すると、それは消える。英数で判定中の英字 (変換中の表示が無い) の
    // あいだに作り直すと、その英字を変換し直せない。どちらも wayland と X11 の入力欄で交互に打たないと起きない。
    void reloadIfLearnedElsewhere() {
        if (!session_ || preeditVisible_ || native_.dataDirectory.empty() ||
            learnedTime(native_.dataDirectory) == seenTime_) {
            return;
        }
        // 先に作る。作れなければ古いセッションのまま使い続ける (学習が消えることはあるが、入力はできる)。
        void *fresh = native_.create();
        if (!fresh) {
            FCITX_ERROR() << "Meltype: 入力欄のセッションを作り直せません (前のセッションのまま使います)";
            return;
        }
        native_.destroy(session_);
        session_ = fresh;
        native_.setDirect(session_, direct_ ? 1 : 0);
        noteLearned();
    }

    // 本体を呼んだ後 (保存したかもしれない): この時点の学習を読んだものとする。
    void noteLearned() {
        if (!native_.dataDirectory.empty()) {
            seenTime_ = learnedTime(native_.dataDirectory);
        }
    }

    void keyEvent(KeyEvent &event) {
        if (event.isRelease() || !ensureSession()) {
            return;
        }
        reloadIfLearnedElsewhere();
        syncCanDelete();
        const Key key = mappedKey(event);
        if (!key.isValid()) {
            return; // 置き換え先が空: 本体に渡さず、押したキーのままアプリへ
        }
        const KeySym sym = key.sym();
        // 半角/全角: 英数 (直接入力) ⇔ 日本語。英数・ひらがなのキーでも切り替える (JIS キーボード)。
        if (isModeKey(sym)) {
            const bool toggle = sym == FcitxKey_Zenkaku_Hankaku || sym == FcitxKey_Hankaku || sym == FcitxKey_Zenkaku;
            const bool toDirect = sym == FcitxKey_Eisu_toggle || sym == FcitxKey_Muhenkan || (toggle && !direct_);
            apply(native_.commit(session_));
            direct_ = toDirect;
            native_.setDirect(session_, toDirect ? 1 : 0);
            event.filterAndAccept();
            return;
        }
        const auto [vk, character] = virtualKey(sym);
        if (!vk) {
            return;
        }
        const KeyStates states = key.states();
        int modifiers = 0;
        if (states.test(KeyState::Shift)) {
            modifiers |= 1;
        }
        if (states.test(KeyState::Ctrl)) {
            modifiers |= 2;
        }
        if (states.test(KeyState::Alt)) {
            modifiers |= 4;
        }
        if (states.test(KeyState::Super) || states.test(KeyState::Super2)) {
            modifiers |= 8;
        }
        std::optional<std::string> before, after;
        if (!preeditVisible_) {
            surroundingText(before, after);
        }
        json_object *result = native_.result(native_.handleKey(session_, vk, character, modifiers,
                                                               before ? before->c_str() : nullptr,
                                                               after ? after->c_str() : nullptr));
        if (!result) {
            return;
        }
        apply(result);
        if (boolOf(result, "consumed")) {
            event.filterAndAccept();
        }
        json_object_put(result);
    }

    // 変換中だけ、割り当てに従ってキーを置き換える。変換中でないときはアプリのキー操作を邪魔しない。
    // 押したキー (event.key()) は fcitx5 が正規化していて Control+n は Control+N になるので、割り当ても正規化して比べる。
    Key mappedKey(const KeyEvent &event) const {
        if (preeditVisible_) {
            for (const auto &[from, to] : keyMap_) {
                if (event.key().check(shiftedLetter(from).normalize())) {
                    return shiftedLetter(to);
                }
            }
        }
        return event.rawKey();
    }

    // 未確定の内容を、表示していた文字のまま確定する (フォーカスが外れた・入力メソッドの切り替え・reset)。
    // 本体の確定 (meltype_commit) は変換前だと表示と違う文字になるので、本体を空にするためだけに呼び、結果は捨てる。
    // フォーカスが外れたときの client preedit は fcitx5 か入力欄が確定するので、ここで確定すると 2 回入る。
    void commitShown(bool focusOut) {
        if (!session_) {
            return;
        }
        native_.free(native_.commit(session_));
        // 未確定の文字が無ければ本体は何も保存しない。ここで覚えると、別の入力欄の保存を読まずに読んだことになる。
        const bool saved = preeditVisible_;
        if (preeditVisible_ && !(focusOut && clientPreedit_)) {
            ic_.commitString(preeditText_);
        }
        hidePreedit();
        hideCandidates();
        ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
        if (saved) {
            noteLearned();
        }
    }

    void selectCandidate(int index) {
        if (session_) {
            syncCanDelete();
            json_object *result = native_.result(native_.selectCandidate(session_, index));
            apply(result);
            json_object_put(result);
            noteLearned();
        }
    }

private:
    // 結果を入力欄に出す
    void apply(char *pointer) {
        json_object *result = native_.result(pointer);
        apply(result);
        json_object_put(result);
    }

    void apply(json_object *result) {
        if (!result) {
            return;
        }
        json_object *commits = member(result, "commits");
        if (commits && json_object_is_type(commits, json_type_array)) {
            for (size_t i = 0; i < json_object_array_length(commits); i++) {
                json_object *edit = json_object_array_get_idx(commits, i);
                hidePreedit();
                const int deleteBefore = intOf(edit, "deleteBefore", 0);
                if (deleteBefore > 0) {
                    // 確定し直し: キャレットの前の文字を消してから入れる (入力欄が対応していれば)。
                    ic_.deleteSurroundingText(-deleteBefore, deleteBefore);
                }
                const std::string text = stringOf(edit, "text");
                if (!text.empty()) {
                    ic_.commitString(text);
                }
            }
        }
        json_object *view = member(result, "view");
        if (view && json_object_is_type(view, json_type_object)) {
            showPreedit(view);
            showCandidates(view);
        } else {
            hidePreedit();
            hideCandidates();
        }
        ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void showPreedit(json_object *view) {
        const std::string text = stringOf(view, "text");
        const std::vector<std::string> clauses = stringsOf(view, "clauses");
        Text preedit;
        size_t cursor = text.size();
        if (boolOf(view, "converting") && !clauses.empty()) {
            // 変換中: 文節ごとに下線。選んでいる文節は強調する (色はテーマに任せる)。
            const int selected = intOf(view, "selectedClause", -1);
            size_t position = 0;
            for (size_t index = 0; index < clauses.size() && position < text.size(); index++) {
                const size_t length = std::min(clauses[index].size(), text.size() - position);
                TextFormatFlags format = TextFormatFlag::Underline;
                if (static_cast<int>(index) == selected) {
                    format |= TextFormatFlag::HighLight;
                    // カーソルは選んでいる文節の先頭に置く (Mozc と同じ)。末尾に置くと候補の長さで候補ウィンドウが動き、
                    // マウスのポインターの下で動くと、強調が上下キーで選んだ候補でなくポインターの下の候補になる
                    cursor = position;
                }
                preedit.append(text.substr(position, length), format);
                position += length;
            }
            if (position < text.size()) {
                preedit.append(text.substr(position), TextFormatFlag::NoFlag);
            }
        } else if (!text.empty()) {
            preedit.append(text, TextFormatFlag::Underline);
        }
        preedit.setCursor(static_cast<int>(cursor));
        clientPreedit_ = ic_.capabilityFlags().test(CapabilityFlag::Preedit);
        if (clientPreedit_) {
            ic_.inputPanel().setClientPreedit(preedit);
        } else {
            ic_.inputPanel().setPreedit(preedit);
        }
        ic_.updatePreedit();
        preeditVisible_ = !text.empty();
        preeditText_ = text;
        // 案内 (Space で変換 など) は補助テキストに出す
        std::string hint = stringOf(view, "hint");
        // もしかして (書き間違い) は案内の先頭に出す (Tab で直せる)
        const std::string suggestion = stringOf(view, "suggestion");
        if (!suggestion.empty()) {
            hint = suggestion + "　" + hint;
        }
        ic_.inputPanel().setAuxUp(Text(hint));
    }

    void hidePreedit() {
        if (preeditVisible_) {
            ic_.inputPanel().setClientPreedit(Text());
            ic_.inputPanel().setPreedit(Text());
            ic_.updatePreedit();
            preeditVisible_ = false;
        }
        ic_.inputPanel().setAuxUp(Text());
    }

    void showCandidates(json_object *view) {
        const std::vector<std::string> candidates = stringsOf(view, "candidates");
        if (!(boolOf(view, "converting") && candidates.size() > 1)) {
            hideCandidates();
            return;
        }
        auto list = std::make_unique<CommonCandidateList>();
        list->setPageSize(9);
        list->setLayoutHint(CandidateLayoutHint::Vertical);
        for (size_t i = 0; i < candidates.size(); i++) {
            list->append<MeltypeCandidate>(this, static_cast<int>(i), candidates[i]);
        }
        const int selected = intOf(view, "selectedIndex", 0);
        if (selected >= 0 && selected < static_cast<int>(candidates.size())) {
            list->setGlobalCursorIndex(selected);
            list->setPage(selected / list->pageSize());
        }
        ic_.inputPanel().setCandidateList(std::move(list));
        scheduleMeaning(view);
    }

    // 候補で少し (1.5 秒) 止まったら、その候補の意味を補助テキストに出す (Windows 版と同じ)
    void scheduleMeaning(json_object *view) {
        const std::string meaning = stringOf(view, "meaning");
        std::optional<std::string> key;
        if (!meaning.empty()) {
            key = std::to_string(intOf(view, "selectedIndex", 0)) + ":" + meaning;
        }
        if (key == meaningKey_) {
            return;
        }
        meaningKey_ = key;
        meaningTimer_.reset();
        if (!key) {
            return;
        }
        meaningTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 1500000, 0,
            [this, meaning](EventSourceTime *, uint64_t) {
                ic_.inputPanel().setAuxUp(Text(meaning));
                ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
                return true;
            });
    }

    void hideCandidates() {
        meaningKey_.reset();
        meaningTimer_.reset();
        ic_.inputPanel().setCandidateList(nullptr);
    }

    // 確定し直し (deleteBefore) は、入力欄が周りの文字に対応していて読めるときだけ本体にさせる。
    // 消せない入力欄で確定し直すと、元の文字が残ったまま書き足される (api → apiあぴ)。
    // 結果を入力欄に出す本体の呼び出しの前に毎回伝える (入力欄を移った最初のキーで、前の入力欄の値のまま確定し直さないように)。
    void syncCanDelete() const {
        if (native_.setCanDelete) {
            const bool canDelete =
                ic_.capabilityFlags().test(CapabilityFlag::SurroundingText) && ic_.surroundingText().isValid();
            native_.setCanDelete(session_, canDelete ? 1 : 0);
        }
    }

    // 入力欄のキャレットの前後の文字列 (それぞれ 20 文字まで)。入力欄が対応していなければ入れない。
    void surroundingText(std::optional<std::string> &before, std::optional<std::string> &after) const {
        if (!ic_.capabilityFlags().test(CapabilityFlag::SurroundingText)) {
            return;
        }
        const SurroundingText &surrounding = ic_.surroundingText();
        if (!surrounding.isValid()) {
            return;
        }
        const std::string &value = surrounding.text();
        const size_t length = utf8::lengthValidated(value);
        if (value.empty() || length == utf8::INVALID_LENGTH) {
            return;
        }
        const size_t cursor = std::min<size_t>(surrounding.cursor(), length);
        auto offset = [&value](size_t characters) {
            return static_cast<size_t>(utf8::ncharByteLength(value.begin(), characters));
        };
        const size_t start = offset(cursor >= 20 ? cursor - 20 : 0);
        const size_t middle = offset(cursor);
        const size_t end = offset(std::min(length, cursor + 20));
        before = value.substr(start, middle - start);
        after = value.substr(middle, end - middle);
    }

    const Native &native_;
    const KeyMap &keyMap_;
    Instance *instance_;
    InputContext &ic_;
    void *session_;
    bool direct_ = false;
    bool preeditVisible_ = false;
    bool clientPreedit_ = false;
    bool createFailed_ = false;
    std::filesystem::file_time_type seenTime_;
    std::string preeditText_;
    std::optional<std::string> meaningKey_;
    std::unique_ptr<EventSourceTime> meaningTimer_;
};

void MeltypeCandidate::select(InputContext *) const { state_->selectCandidate(index_); }

// ---- 入力メソッド ----

class MeltypeEngine : public InputMethodEngine {
public:
    explicit MeltypeEngine(Instance *instance)
        : instance_(instance),
          factory_([this](InputContext &ic) { return new MeltypeState(native_, keyMap_, instance_, ic); }) {
        native_.load();
        reloadConfig();
        instance_->inputContextManager().registerProperty("meltypeState", &factory_);
    }

    void keyEvent(const InputMethodEntry &, KeyEvent &event) override {
        MeltypeState *current = state(event.inputContext());
        current->keyEvent(event);
        current->noteLearned();
    }

    void activate(const InputMethodEntry &, InputContextEvent &event) override {
        MeltypeState *current = state(event.inputContext());
        if (current->ensureSession()) {
            current->reloadIfLearnedElsewhere();
        }
    }

    // 別の入力欄・アプリに移るとき、入力メソッドを切り替えたときは、表示していた未確定の文字を確定する。
    // 本体の確定 (meltype_commit) の結果は捨てる (commitShown)。
    void deactivate(const InputMethodEntry &, InputContextEvent &event) override {
        state(event.inputContext())->commitShown(event.type() == EventType::InputContextFocusOut);
    }

    void reset(const InputMethodEntry &, InputContextEvent &event) override {
        state(event.inputContext())->commitShown(false);
    }

    // アドオン設定は keymap.conf と同期する (保存先はこのファイルだけ)。手で直したら fcitx5 の再起動で読み直す
    // (fcitx5-remote -r は全体の設定だけを読み直し、ここは呼ばない)。
    void reloadConfig() override {
        keyMap_ = readKeyMap(keyMapPath());
        fillConfig();
    }
    const Configuration *getConfig() const override { return &config_; }
    // 画面の欄 (ACTIONS の置き換え先) の分だけを入れ替える。ほかの置き換え先の組は keymap.conf のまま残す。
    // 同じ押すキーが複数の行にあると先の行が効くので、残す行の順は変えない (消した行は抜き、増えた行は末尾に足す)。
    void setConfig(const RawConfig &raw) override {
        config_.load(raw);
        std::vector<KeyList> added(std::size(ACTIONS));
        for (size_t i = 0; i < std::size(ACTIONS); i++) {
            std::ranges::copy_if(config_.keys[i]->value(), std::back_inserter(added[i]),
                                 [](const Key &key) { return key.isValid(); });
        }
        std::erase_if(keyMap_, [&](const auto &entry) {
            for (size_t i = 0; i < std::size(ACTIONS); i++) {
                if (sameKey(entry.second, Key(ACTIONS[i].key))) {
                    // 画面に残っている押すキーなら、行はそのまま残し、足す側からは外す
                    auto kept = std::ranges::find_if(added[i], [&](const Key &key) {
                        return shiftedLetter(key).normalize() == shiftedLetter(entry.first).normalize();
                    });
                    if (kept == added[i].end()) {
                        return true;
                    }
                    added[i].erase(kept);
                    return false;
                }
            }
            return false;
        });
        for (size_t i = 0; i < std::size(ACTIONS); i++) {
            for (const Key &from : added[i]) {
                keyMap_.emplace_back(from, Key(ACTIONS[i].key));
            }
        }
        fillConfig();
        writeKeyMap(keyMapPath(), keyMap_);
    }

private:
    MeltypeState *state(InputContext *ic) { return ic->propertyFor(&factory_); }

    // 画面の欄に、keymap.conf の組を置き換え先ごとに分けて入れる
    void fillConfig() {
        for (size_t i = 0; i < std::size(ACTIONS); i++) {
            KeyList keys;
            for (const auto &[from, to] : keyMap_) {
                if (sameKey(to, Key(ACTIONS[i].key))) {
                    keys.push_back(from);
                }
            }
            config_.keys[i]->setValue(keys);
        }
    }

    Instance *instance_;
    Native native_;
    KeyMap keyMap_;
    MeltypeConfig config_;
    FactoryFor<MeltypeState> factory_;
};

class MeltypeEngineFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override { return new MeltypeEngine(manager->instance()); }
};

} // namespace

FCITX_ADDON_FACTORY_V2(meltype, MeltypeEngineFactory)
