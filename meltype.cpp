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
#include <dlfcn.h>
#include <json-c/json.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>
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
    void (*free)(char *) = nullptr;

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
    MeltypeState(const Native &native, Instance *instance, InputContext &ic)
        : native_(native), instance_(instance), ic_(ic),
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
        }
        return session_;
    }

    void keyEvent(KeyEvent &event) {
        if (event.isRelease() || !ensureSession()) {
            return;
        }
        const Key key = event.rawKey();
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

    // 未確定の内容を、表示していた文字のまま確定する (フォーカスが外れた・入力メソッドの切り替え・reset)。
    // 本体の確定 (meltype_commit) は変換前だと表示と違う文字になるので、本体を空にするためだけに呼び、結果は捨てる。
    // フォーカスが外れたときの client preedit は fcitx5 か入力欄が確定するので、ここで確定すると 2 回入る。
    void commitShown(bool focusOut) {
        if (!session_) {
            return;
        }
        native_.free(native_.commit(session_));
        if (preeditVisible_ && !(focusOut && clientPreedit_)) {
            ic_.commitString(preeditText_);
        }
        hidePreedit();
        hideCandidates();
        ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void selectCandidate(int index) {
        if (session_) {
            json_object *result = native_.result(native_.selectCandidate(session_, index));
            apply(result);
            json_object_put(result);
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
        if (boolOf(view, "converting") && !clauses.empty()) {
            // 変換中: 文節ごとに下線。選んでいる文節は強調する (色はテーマに任せる)。
            const int selected = intOf(view, "selectedClause", -1);
            size_t position = 0;
            for (size_t index = 0; index < clauses.size() && position < text.size(); index++) {
                const size_t length = std::min(clauses[index].size(), text.size() - position);
                TextFormatFlags format = TextFormatFlag::Underline;
                if (static_cast<int>(index) == selected) {
                    format |= TextFormatFlag::HighLight;
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
        preedit.setCursor(static_cast<int>(text.size()));
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
    Instance *instance_;
    InputContext &ic_;
    void *session_;
    bool direct_ = false;
    bool preeditVisible_ = false;
    bool clientPreedit_ = false;
    bool createFailed_ = false;
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
          factory_([this](InputContext &ic) { return new MeltypeState(native_, instance_, ic); }) {
        native_.load();
        instance_->inputContextManager().registerProperty("meltypeState", &factory_);
    }

    void keyEvent(const InputMethodEntry &, KeyEvent &event) override {
        state(event.inputContext())->keyEvent(event);
    }

    void activate(const InputMethodEntry &, InputContextEvent &event) override {
        state(event.inputContext())->ensureSession();
    }

    // 別の入力欄・アプリに移るとき、入力メソッドを切り替えたときは、表示していた未確定の文字を確定する。
    // 本体の確定 (meltype_commit) の結果は捨てる (commitShown)。
    void deactivate(const InputMethodEntry &, InputContextEvent &event) override {
        state(event.inputContext())->commitShown(event.type() == EventType::InputContextFocusOut);
    }

    void reset(const InputMethodEntry &, InputContextEvent &event) override {
        state(event.inputContext())->commitShown(false);
    }

private:
    MeltypeState *state(InputContext *ic) { return ic->propertyFor(&factory_); }

    Instance *instance_;
    Native native_;
    FactoryFor<MeltypeState> factory_;
};

class MeltypeEngineFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override { return new MeltypeEngine(manager->instance()); }
};

} // namespace

FCITX_ADDON_FACTORY_V2(meltype, MeltypeEngineFactory)
