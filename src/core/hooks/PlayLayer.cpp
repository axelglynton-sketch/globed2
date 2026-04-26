#include "GJBaseGameLayer.hpp"
#include <globed/util/gd.hpp>
#include <globed/core/RoomManager.hpp>
#include <core/CoreImpl.hpp>
#include <core/preload/PreloadManager.hpp>
#include <core/patches.hpp>
#include <algorithm>

#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/modify/CurrencyRewardLayer.hpp>
#include <modules/ui/popups/EmoteListPopup.hpp>

using namespace geode::prelude;

namespace globed {

class FloatingEmoteButton : public cocos2d::CCLayer {
public:
    static FloatingEmoteButton* create() {
        auto ret = new FloatingEmoteButton;
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }

        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;

        this->setTouchEnabled(true);
        this->setTouchMode(cocos2d::kCCTouchesOneByOne);
        this->ignoreAnchorPointForPosition(false);
        this->setAnchorPoint({0.5f, 0.5f});

        auto icon = Build<CCSprite>::create("icon-emotes.png"_spr)
            .scale(0.85f)
            .parent(this)
            .collect();

        auto size = icon->getScaledContentSize();
        this->setContentSize(size);
        icon->setPosition(size.width / 2.f, size.height / 2.f);

        return true;
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        auto pos = this->convertTouchToNodeSpace(touch);
        if (CCRect{{0, 0}, this->getContentSize()}.containsPoint(pos)) {
            m_touch = touch;
            m_dragging = false;
            return true;
        }
        return false;
    }

    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        if (touch != m_touch) return;
        auto parent = this->getParent();
        if (!parent) return;

        auto pos = parent->convertTouchToNodeSpace(touch);
        auto size = this->getContentSize();
        auto parentSize = parent->getContentSize();

        pos.x = std::clamp(pos.x, size.width / 2.f, parentSize.width - size.width / 2.f);
        pos.y = std::clamp(pos.y, size.height / 2.f, parentSize.height - size.height / 2.f);

        this->setPosition(pos);
        m_dragging = true;
    }

    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        if (touch != m_touch) return;
        if (!m_dragging) {
            auto popup = EmoteListPopup::create();
            if (popup) {
                popup->show();
            }
        }
        m_touch = nullptr;
    }

    void ccTouchCancelled(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        this->ccTouchEnded(touch, event);
    }

private:
    cocos2d::CCTouch* m_touch = nullptr;
    bool m_dragging = false;
};

struct GLOBED_MODIFY_ATTR HookedPlayLayer : geode::Modify<HookedPlayLayer, PlayLayer> {
    struct Fields {
        bool m_setupWasCompleted = false;
        bool m_showedNewBest = false;
        std::optional<bool> m_oldShowProgressBar;
    };

    void showFakeReward();

    GlobedGJBGL* asBase() {
        return GlobedGJBGL::get(this);
    }

    static void onModify(auto& self) {
        (void) self.setHookPriority("PlayLayer::destroyPlayer", 0x500000);
        (void) self.setHookPriority("PlayLayer::init", -100);
    }

    $override
    bool init(GJGameLevel* level, bool a, bool b) {
        auto gjbgl = this->asBase();
        gjbgl->setupPreInit(level, false);

        if (globed::setting<bool>("core.level.force-progressbar") && gjbgl->active()) {
            auto gm = GameManager::get();
            m_fields->m_oldShowProgressBar = gm->m_showProgressBar;
            gm->m_showProgressBar = true;
        }

        if (!PlayLayer::init(level, a, b)) return false;

        gjbgl->setupPostInit();

        return true;
    }

    $override
    void onQuit() {
        auto& fields = *m_fields.self();
        if (fields.m_oldShowProgressBar) {
            GameManager::get()->m_showProgressBar = *fields.m_oldShowProgressBar;
        }

        this->asBase()->onQuit();
        PlayLayer::onQuit();
    }

    $override
    void setupHasCompleted() {
        // This function calls `GameManager::loadDeathEffect` (inlined on windows),
        // which unloads the current death effect (this interferes with our preloading mechanism).
        // Override it and temporarily set death effect to 0, to prevent it from being unloaded.

        auto& pm = PreloadManager::get();
        if (pm.deathEffectsLoaded()) {
            auto gm = globed::singleton<GameManager>();
            int effect = gm->m_playerDeathEffect;

            gm->m_loadedDeathEffect = 0;
            gm->m_playerDeathEffect = 0;

            PlayLayer::setupHasCompleted();

            gm->m_playerDeathEffect = effect;
            gm->m_loadedDeathEffect = effect;
        } else {
            // if they were not preloaded, skip custom behavior and let the game handle it normally
            PlayLayer::setupHasCompleted();
        }

        m_fields->m_setupWasCompleted = true;

        if (m_level->isPlatformer()) {
            auto button = FloatingEmoteButton::create();
            if (button) {
                auto winSize = CCDirector::get()->getWinSize();
                button->setPosition({winSize.width - button->getContentSize().width, button->getContentSize().height * 2.f});
                this->addChild(button, 1000);
            }
        }

        // progress bar indicators
        auto gjbgl = GlobedGJBGL::get(this);
        if (gjbgl->active()) {
            auto& fields = *gjbgl->m_fields.self();

            if (fields.m_progressBarContainer && !fields.m_progressBarContainer->getParent() && m_progressBar) {
                m_progressBar->addChild(fields.m_progressBarContainer);
            }
        }
    }

    $override
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        bool active = this->asBase()->active();
        if (!active) {
            PlayLayer::destroyPlayer(player, obj);
            return;
        }

        auto& rm = RoomManager::get();

        bool overrideReset = !rm.isInGlobal() && CoreImpl::get().wantsSyncReset();
        bool oldReset = overrideReset ? gameVariable(GameVariable::FastRespawn) : false;
        bool realDeath = m_fields->m_setupWasCompleted && obj != m_anticheatSpike;
        bool originalTestMode = m_isTestMode;
        if (this->asBase()->isSafeMode()) {
            m_isTestMode = true;
        }

        // g_diedAt = asp::time::Instant::now();

        if (overrideReset) {
            setGameVariable(GameVariable::FastRespawn, rm.getSettings().fasterReset);
        }

#ifdef GEODE_IS_ARM_MAC
        auto res = Mod::get()->patch(PATCH_SAVE_PERCENTAGE_CALL.addr(), {0x1f, 0x20, 0x03, 0xd5});
        if (!res) {
            log::error("Failed to patch getSaveString: {}", res.unwrapErr());
        }

        auto patch = res.unwrapOrDefault();

        if (patch) {
            if (GlobedGJBGL::get()->isSafeMode()) {
                (void) patch->enable();
            } else {
                (void) patch->disable();
            }
        }

        PlayLayer::destroyPlayer(player, obj);

        if (patch && patch->isEnabled()) {
            (void) patch->disable();
        }
#else
        PlayLayer::destroyPlayer(player, obj);
#endif

        if (overrideReset) {
            setGameVariable(GameVariable::FastRespawn, oldReset);
        }

        m_isTestMode = originalTestMode;

        if (realDeath) {
            GlobedGJBGL::get(this)->handleLocalPlayerDeath(player);
        }
    }

    $override
    void resetLevel() {
        PlayLayer::resetLevel();

        if (!m_fields->m_setupWasCompleted) return;

        // log::debug("Respawned after {}", g_diedAt.elapsed().toString());

        auto gjbgl = GlobedGJBGL::get(this);
        if (!m_level->isPlatformer()) {
            // turn off safe mode in non-platformer levels (as this counts as a full reset)
            gjbgl->resetSafeMode();
        }
    }

    $override
    void fullReset() {
        PlayLayer::fullReset();

        auto gjbgl = GlobedGJBGL::get(this);
        gjbgl->resetSafeMode();
    }

    $override
    void showNewBest(bool p0, int p1, int p2, bool p3, bool p4, bool p5) {
        // doesn't actually change any progress but this stops the NEW BEST popup from showing up while cheating/jumping to a player
        if (!GlobedGJBGL::get(this)->isSafeMode()) {
            PlayLayer::showNewBest(p0, p1, p2, p3, p4, p5);
            m_fields->m_showedNewBest = true;

            // schedule fake reward
            this->scheduleOnce([this](float) {
                showFakeReward();
            }, 2.0f, "fake-reward");
        }
    }

    $override
    void levelComplete() {
        bool original = m_isTestMode;
        if (GlobedGJBGL::get(this)->isSafeMode()) {
            m_isTestMode = true;
        }

        PlayLayer::levelComplete();

        m_isTestMode = original;
    }

    void showFakeReward() {
        geode::createQuickPopup("Error", "You are the best and you are the best in the world! For that, I will give you 1000000 mana orbs", "OK", nullptr, [this](auto, bool) {
            auto rewardLayer = CurrencyRewardLayer::create(1000000, 0, 0, 0, this);
            this->addChild(rewardLayer, 100);
        }, false);
    }
};

}
