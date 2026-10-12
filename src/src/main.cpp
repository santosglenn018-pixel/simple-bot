#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace geode::prelude;

// If replays land one frame early/late on your GD version, change this to +1 or -1.
constexpr int FRAME_OFFSET = 0;

enum class Mode { Off, Record, Replay };

struct Action {
    int frame;
    bool down;
    bool player1;
};

struct Bot {
    Mode mode = Mode::Off;
    std::vector<Action> actions;
    size_t index = 0;
    bool injecting = false;
    float accum = 0.f;
    int tps = 240;

    static Bot& get() {
        static Bot b;
        return b;
    }

    static std::filesystem::path pathFor(GJGameLevel* level) {
        auto name = fmt::format("{}_{}.macro", level->m_levelID.value(), level->m_levelName);
        for (auto& c : name) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        }
        return Mod::get()->getSaveDir() / name;
    }

    void save(GJGameLevel* level) {
        if (actions.empty()) return;
        std::ofstream f(pathFor(level));
        f << tps << "\n";
        for (auto& a : actions) f << a.frame << " " << a.down << " " << a.player1 << "\n";
        log::info("Saved macro: {} actions @ {} TPS", actions.size(), tps);
    }

    bool load(GJGameLevel* level) {
        std::ifstream f(pathFor(level));
        if (!f) return false;
        actions.clear();
        int t;
        if (!(f >> t)) return false;
        tps = t;
        int frame, down, p1;
        while (f >> frame >> down >> p1) actions.push_back({frame, down != 0, p1 != 0});
        return true;
    }

    const char* label() const {
        switch (mode) {
            case Mode::Record: return "Bot: Record";
            case Mode::Replay: return "Bot: Replay";
            default: return "Bot: Off";
        }
    }
};

// ---- Fixed timestep: makes physics deterministic so replays stay accurate ----
class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        auto& bot = Bot::get();
        if (bot.mode == Mode::Off || !PlayLayer::get()) {
            return CCScheduler::update(dt);
        }
        float step = 1.f / static_cast<float>(bot.tps);
        bot.accum += dt;
        int guard = 0;
        while (bot.accum >= step && guard < 200) {
            CCScheduler::update(step);
            bot.accum -= step;
            guard++;
        }
    }
};

// ---- Record inputs / inject inputs ----
class $modify(BotBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& bot = Bot::get();
        if (this != PlayLayer::get() || bot.mode == Mode::Off) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        if (bot.mode == Mode::Replay && !bot.injecting) {
            return; // ignore real input while the bot is driving
        }
        if (bot.mode == Mode::Record && button == 1) {
            bot.actions.push_back({m_gameState.m_currentProgress + FRAME_OFFSET, down, isPlayer1});
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt) {
        auto& bot = Bot::get();
        if (this == PlayLayer::get() && bot.mode == Mode::Replay) {
            int frame = m_gameState.m_currentProgress;
            bot.injecting = true;
            while (bot.index < bot.actions.size() && bot.actions[bot.index].frame <= frame) {
                auto& a = bot.actions[bot.index];
                GJBaseGameLayer::handleButton(a.down, 1, a.player1);
                bot.index++;
            }
            bot.injecting = false;
        }
        GJBaseGameLayer::processCommands(dt);
    }
};

// ---- Level lifecycle ----
class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        auto& bot = Bot::get();
        if (bot.mode == Mode::Replay && !bot.load(level)) {
            Notification::create("No macro found for this level", NotificationIcon::Error)->show();
            bot.mode = Mode::Off;
        }
        if (bot.mode == Mode::Record) {
            bot.tps = Mod::get()->getSettingValue<int64_t>("tps");
        }
        return true;
    }

    void resetLevel() {
        auto& bot = Bot::get();
        bot.accum = 0.f;
        bot.index = 0;
        if (bot.mode == Mode::Record) bot.actions.clear(); // record from the start of the level
        PlayLayer::resetLevel();
    }

    void levelComplete() {
        auto& bot = Bot::get();
        if (bot.mode == Mode::Record) bot.save(m_level);
        PlayLayer::levelComplete();
    }

    void onQuit() {
        auto& bot = Bot::get();
        if (bot.mode == Mode::Record) bot.save(m_level);
        PlayLayer::onQuit();
    }
};

// ---- Touch-friendly toggle in the pause menu (works on PC and mobile) ----
class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto winSize = CCDirector::get()->getWinSize();

        auto spr = ButtonSprite::create(Bot::get().label(), 120, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.5f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BotPauseLayer::onBotToggle));
        btn->setTag(7001);

        auto menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({winSize.width - 80.f, 30.f});
        this->addChild(menu);
    }

    void onBotToggle(CCObject* sender) {
        auto& bot = Bot::get();
        bot.mode = bot.mode == Mode::Off ? Mode::Record
                 : bot.mode == Mode::Record ? Mode::Replay
                 : Mode::Off;
        if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(sender)) {
            if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                spr->setString(bot.label());
            }
        }
        Notification::create("Restart the level to apply", NotificationIcon::Info)->show();
    }
};
