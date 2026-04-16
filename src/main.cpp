#include <Geode/Geode.hpp>
#include <Geode/modify/CCScene.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#include "BlurAPI.hpp"
#include <box2d/box2d.h>

#include <vector>
#include <cmath>
#include <cstdlib>
#include <ctime>

using namespace geode::prelude;
using namespace cocos2d;

// forward decls so the hooks below can call these
static void resetIdle();
static void dismissActive();

// global state, yes i know
static bool  s_inPlayLayer = false;
static float s_idleTimer   = 0.f;
static bool  s_ssActive    = false;

// pixels per meter for box2d
static constexpr float PPM           = 40.f;
static constexpr float FADE_DURATION = 0.5f;

// fixed
// 36,     84,   141,   1022,  1330,  1333,  1704,  1751,  3004,  3027,  1594
// yellow, blue, pink, green, black, red, dash, dash2, spider, teleport, toggle
static const int   ORB_IDS[11]       = { 36,    84,    141,   1022,   1330,  1333,  1704, 1751, 3004, 3027,  1594 };
static const float ORB_FULL_SIZE[11] = { 32.3f, 33.2f, 33.6f, 31.96f, 36.5f, 34.9f, 41.f, 41.f, 41.f, 39.4f, 30.8f };
// measured on device, trust
static constexpr float PLAYER_FULL_SIZE = 35.f;

enum class BgMode { Color, Blur };

// screensaverLayer - i hate this shit
class ScreensaverLayer : public CCLayerColor {
public:
    static ScreensaverLayer* create() {
        auto* ret = new ScreensaverLayer();
        // tag 2047 so dismissActive can find it later without keeping a pointer
        if (ret && ret->init()) { ret->autorelease(); ret->setTag(2047); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    void dismiss() {
        if (m_dismissed) return;
        m_dismissed = true;
        this->stopAllActions();
        this->unscheduleAllSelectors();
        // removes itself, no need to call from outside
        this->removeFromParentAndCleanup(true);
        s_ssActive  = false;
        s_idleTimer = 0.f;
    }

private:
    // one ball = one physics body + one visual node
    struct Ball {
        b2Body* body  = nullptr;
        float   radius = 0.f;
        int     orbIdx = 0;
        bool    isPlayer = false;
        CCNode* node  = nullptr;
    };

    b2World*          m_world   = nullptr;
    b2Body*           m_wallBot = nullptr; // kept it separate so we can remove it when draining
    std::vector<Ball> m_balls;

    int  m_globalTime    = 0;
    int  m_nextSpawn     = 0;
    bool m_playerSpawned = false;
    bool m_fillingDone   = false;
    bool m_draining      = false;
    bool m_dismissed     = false;

    float m_physAccum  = 0.f;
    float m_drainDelay = 3.f;

    // these get overwritten from settings in startCycle
    int     m_numBalls   = 120;
    float   m_speedMult  = 1.f;
    int     m_cubeChance = 50;
    bool    m_noGround   = false;
    int     m_dropTime   = 2;
    BgMode  m_bgMode     = BgMode::Color;
    ccColor4B m_bgColor  = {0, 0, 0, 255};

    bool  m_fadeDone  = false;
    float m_fadeTimer = 0.f;

    CCSize m_screen;

    bool init() override {
        auto* mod = Mod::get();
        m_bgColor = mod->getSettingValue<ccColor4B>("bg-color");

        bool wantBlur = mod->getSettingValue<bool>("bg-blur");
        // only use blur if BlurAPI is actually loaded
        m_bgMode = (wantBlur && BlurAPI::isBlurAPIEnabled()) ? BgMode::Blur : BgMode::Color;

        // start transparent, fade in during update
        ccColor4B startColor = {m_bgColor.r, m_bgColor.g, m_bgColor.b, 0};
        if (!CCLayerColor::initWithColor(startColor)) return false;

        m_screen = CCDirector::get()->getWinSize();
        this->setContentSize(m_screen);
        this->setPosition(CCPointZero);

        if (m_bgMode == BgMode::Blur)
            BlurAPI::addBlur(this);

        // need all of these or input wont get eaten
        this->setTouchEnabled(true);
        this->setTouchMode(kCCTouchesOneByOne);
        this->setTouchPriority(-9999);
        this->setKeypadEnabled(true);
        this->setKeyboardEnabled(true);
#ifdef GEODE_IS_DESKTOP
        this->setMouseEnabled(true);
#endif
        this->scheduleUpdate();
        std::srand((unsigned)std::time(nullptr));
        startCycle();
        return true;
    }

    // any input = dismiss
    bool ccTouchBegan(CCTouch*, CCEvent*) override { dismiss(); return true; }
    void keyDown(enumKeyCodes, double) override    { dismiss(); }
    void keyBackClicked() override                 { dismiss(); }
#ifdef GEODE_IS_DESKTOP
    void scrollWheel(float, float) override        { dismiss(); }
#endif

    void startCycle() {
        auto* mod   = Mod::get();
        m_numBalls   = (int)mod->getSettingValue<int64_t>("orb-count");
        m_cubeChance = (int)mod->getSettingValue<int64_t>("cube-chance");
        m_noGround   = mod->getSettingValue<bool>("no-ground");
        float rawSpeed = (float)mod->getSettingValue<int64_t>("orb-speed");
        m_speedMult  = rawSpeed / 10.f;
        // faster speed = less frames between drops
        m_dropTime   = std::max(1, (int)(20.f / m_speedMult));

        if (m_numBalls < 1) m_numBalls = 1;

        m_globalTime    = 0;
        m_nextSpawn     = 0;
        m_playerSpawned = false;
        m_fillingDone   = false;
        m_draining      = false;
        m_physAccum     = 0.f;
        // small random delay before the floor drops
        m_drainDelay    = 5.f + (std::rand() % 1001) / 1000.f;

        for (auto& b : m_balls)
            if (b.node) b.node->removeFromParentAndCleanup(true);
        m_balls.clear();

        // rebuild world from scratch each cycle
        delete m_world;
        m_world   = nullptr;
        m_wallBot = nullptr;

        b2Vec2 grav(0.f, 9.8f * m_speedMult * 3.f);
        m_world = new b2World(grav);

        float W = m_screen.width, H = m_screen.height;

        // helper lambda, left/right/bottom walls
        auto makeWall = [&](float x1, float y1, float x2, float y2) -> b2Body* {
            b2BodyDef bd; bd.type = b2_staticBody;
            auto* body = m_world->CreateBody(&bd);
            b2EdgeShape es;
            es.SetTwoSided(b2Vec2(x1/PPM, y1/PPM), b2Vec2(x2/PPM, y2/PPM));
            b2FixtureDef fd; fd.shape = &es; fd.restitution = 0.5f; fd.friction = 0.7f;
            body->CreateFixture(&fd);
            return body;
        };

        makeWall(0, 0, 0, H);
        makeWall(W, 0, W, H);
        if (!m_noGround)
            m_wallBot = makeWall(0, H, W, H); // save ref so we can destroy it later
    }

    void update(float dt) override {
        if (m_dismissed) return;

        // fade in first before doing anything else
        if (!m_fadeDone) {
            m_fadeTimer += dt;
            float t = m_fadeTimer / FADE_DURATION;
            if (t >= 1.f) { t = 1.f; m_fadeDone = true; }
            GLubyte targetA = (m_bgMode == BgMode::Blur) ? 180 : m_bgColor.a;
            this->setOpacity((GLubyte)(targetA * t));
            return;
        }

        float W = m_screen.width, H = m_screen.height;
        m_globalTime++;

        // drop orbs one at a time, staggered by dropTime frames
        while (m_nextSpawn < m_numBalls && m_globalTime >= m_dropTime * m_nextSpawn) {
            spawnOrb(W, H);
            m_nextSpawn++;
        }

        // spawn the player cube roughly halfway through
        if (!m_playerSpawned && m_nextSpawn >= m_numBalls / 2) {
            m_playerSpawned = true;
            if ((std::rand() % 100) < m_cubeChance)
                spawnPlayerCube(W, H);
        }

        // once all orbs are out, wait drainDelay then remove the floor
        if (!m_noGround && !m_fillingDone && m_nextSpawn >= m_numBalls) {
            int drainFrame = m_numBalls * m_dropTime + (int)(m_drainDelay * 60.f);
            if (m_globalTime >= drainFrame) {
                m_fillingDone = true;
                m_draining    = true;
                if (m_wallBot) { m_world->DestroyBody(m_wallBot); m_wallBot = nullptr; }
            }
        }

        // restart once everything fell off screen
        if (!m_noGround && m_draining) {
            bool allOff = true;
            for (auto& b : m_balls)
                if (b.body->GetPosition().y * PPM < H + 300.f) { allOff = false; break; }
            if (allOff) { startCycle(); return; }
        }

        // no-ground mode just restarts after a fixed time
        if (m_noGround && m_globalTime > m_numBalls * m_dropTime + 500) {
            startCycle(); return;
        }

        // fixed timestep accumulator so physics doesnt go nuts on lag spikes
        static constexpr float STEP = 1.f / 60.f;
        m_physAccum += dt;
        while (m_physAccum >= STEP) {
            m_world->Step(STEP, 8, 3);
            m_physAccum -= STEP;
        }

        // sync visual nodes to physics bodies, box2d y is flipped compared to cocos so H - py
        for (auto& b : m_balls) {
            if (!b.node) continue;
            auto p = b.body->GetPosition();
            b.node->setPosition({p.x * PPM, H - p.y * PPM});
            b.node->setRotation(b.body->GetAngle() * (180.f / (float)M_PI));
        }
    }

    void spawnOrb(float W, float) {
        int   orbIdx = std::rand() % 11;
        bool  isBox  = (orbIdx == 10); // toggle orb is the only square one
        float half   = ORB_FULL_SIZE[orbIdx] * 0.5f;

        b2BodyDef bd;
        bd.type = b2_dynamicBody;
        bd.position.Set(
            (W * 0.1f + (std::rand() % std::max(1, (int)(W * 0.8f)))) / PPM,
            -250.f / PPM // spawn above screen
        );
        bd.angle = (float)(std::rand() % 360) * ((float)M_PI / 180.f);
        auto* body = m_world->CreateBody(&bd);

        b2FixtureDef   fd;
        b2CircleShape  cs;
        b2PolygonShape ps;
        fd.density = 1.f; fd.restitution = 0.5f; fd.friction = 1.f;

        if (isBox) {
            ps.SetAsBox(half/PPM, half/PPM);
            fd.shape = &ps;
        } else {
            cs.m_radius = half / PPM;
            fd.shape    = &cs;
        }
        body->CreateFixture(&fd);
        // tiny random horizontal nudge so they dont all stack perfectly
        body->ApplyLinearImpulse(
            b2Vec2((10 - std::rand() % 21) * 0.05f, 0.f),
            body->GetWorldCenter(), true
        );

        auto* obj = GameObject::createWithKey(ORB_IDS[orbIdx]);
        if (obj) {
            this->addChild(obj, 2);
            obj->setScale(1.f);
            obj->setPosition({-9999.f, -9999.f}); // update() moves it every frame
        }

        m_balls.push_back({body, half, orbIdx, false, obj});
    }

    void spawnPlayerCube(float W, float) {
        auto* gm = GameManager::get();
        float half = PLAYER_FULL_SIZE * 0.5f;

        b2BodyDef bd;
        bd.type = b2_dynamicBody;
        bd.position.Set(
            ((float)(std::rand() % std::max(1, (int)W))) / PPM,
            -(200.f + std::rand() % 800) / PPM // higher up than orbs cuz once it got fucked
        );
        bd.angle = (float)(std::rand() % 360) * ((float)M_PI / 180.f);
        auto* body = m_world->CreateBody(&bd);

        b2PolygonShape ps;
        ps.SetAsBox(half/PPM, half/PPM);
        b2FixtureDef fd;
        fd.shape = &ps; fd.density = 1.f; fd.restitution = 0.5f; fd.friction = 0.7f;
        body->CreateFixture(&fd);

        //  SimplePlayer with colors and glow
        auto* sp = SimplePlayer::create(gm->getPlayerFrame());
        if (sp) {
            sp->setColors(
                gm->colorForIdx(gm->getPlayerColor()),
                gm->colorForIdx(gm->getPlayerColor2())
            );
            if (gm->getPlayerGlow())
                sp->setGlowOutline(gm->colorForIdx(gm->getPlayerGlowColor()));
            this->addChild(sp, 3);
            sp->setScale(1.f);
            sp->setPosition({-9999.f, -9999.f});
        }

        m_balls.push_back({body, half, 0, true, sp});
    }

    ~ScreensaverLayer() override {
        if (m_bgMode == BgMode::Blur)
            BlurAPI::removeBlur(this);
        delete m_world;
        m_world = nullptr;
    }
};

static void resetIdle() { s_idleTimer = 0.f; }

static void dismissActive() {
    auto* scene = CCDirector::get()->getRunningScene();
    if (!scene) return;
    // find by tag instead of keeping a global pointer, thats bette
    if (auto* ss = scene->getChildByTag(2047))
        static_cast<ScreensaverLayer*>(ss)->dismiss();
}

// hook every scene so we get a per-frame tick everywhere
class $modify(OSSCCScene, CCScene) {
    bool init() {
        if (!CCScene::init()) return false;
        this->schedule(schedule_selector(OSSCCScene::idleTick), 0.f);
        return true;
    }

    void idleTick(float dt) {
        if (s_ssActive || s_inPlayLayer) { s_idleTimer = 0.f; return; }

        s_idleTimer += dt;
        float timeout = (float)Mod::get()->getSettingValue<int64_t>("idle-timeout");
        if (s_idleTimer < timeout) return;

        s_idleTimer = 0.f;
        s_ssActive  = true;

        auto* ss = ScreensaverLayer::create();
        if (ss) { ss->setZOrder(9999); this->addChild(ss); }
        else s_ssActive = false;
    }
};

// resets idle timer on any touch
class $modify(OSSCCTouchDispatcher, CCTouchDispatcher) {
    void touches(CCSet* touches, CCEvent* event, unsigned int type) {
        CCTouchDispatcher::touches(touches, event, type);
        if (type == CCTOUCHBEGAN) resetIdle();
    }
};

// fuck
// keyboard needs to both reset AND dismiss (touch input is handled by the layer itself)
class $modify(OSSCCKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        bool ret = CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
        if (isKeyDown && !isKeyRepeat) { resetIdle(); dismissActive(); }
        return ret;
    }
};

// dont show screensaver while PlayLayer
class $modify(OSSPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        s_inPlayLayer = true;
        s_idleTimer   = 0.f;
        dismissActive();
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void onQuit() {
        s_inPlayLayer = false;
        s_idleTimer   = 0.f;
        PlayLayer::onQuit();
    }
};