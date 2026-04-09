#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCMouseDispatcher.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/SimplePlayer.hpp>
#include <box2d/box2d.h>

using namespace geode::prelude;

static constexpr float PPM = 50.0f;

struct OrbEntry {
    b2Body*  body;
    CCNode*  node;
    float    radius;
    bool     isBox;
};

static float s_idleSeconds = 0.f;

class OrbitScreensaverLayer : public CCLayerColor {
public:
    b2World*               m_world      = nullptr;
    std::vector<OrbEntry>  m_orbs;
    b2Body*                m_groundBody = nullptr;
    bool                   m_draining   = false;
    float                  m_drainTimer = 0.f;
    static bool            s_active;

    static OrbitScreensaverLayer* create() {
        auto ret = new OrbitScreensaverLayer();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init() {
        auto winSize = CCDirector::get()->getWinSize();
        auto bgCol   = Mod::get()->getSettingValue<ccColor4B>("bg-color");
        if (!CCLayerColor::initWithColor(bgCol, winSize.width, winSize.height))
            return false;

        this->setTouchEnabled(true);
        this->setTouchMode(kCCTouchesAllAtOnce);
        this->setKeypadEnabled(true);

        auto hint = CCLabelBMFont::create("Tap or press any key to dismiss", "chatFont.fnt");
        hint->setScale(0.55f);
        hint->setOpacity(140);
        hint->setPosition(ccp(winSize.width / 2.f, 14.f));
        this->addChild(hint, 10);

        float speedMul = (float)Mod::get()->getSettingValue<double>("speed");
        b2Vec2 gravity(0.0f, -9.8f * speedMul * 3.0f);
        m_world = new b2World(gravity);

        this->buildWalls();
        this->spawnOrbs();
        this->scheduleUpdate();
        return true;
    }

    ~OrbitScreensaverLayer() {
        delete m_world;
    }

    void buildWalls() {
        auto winSize = CCDirector::get()->getWinSize();
        float W = winSize.width  / PPM;
        float H = winSize.height / PPM;

        auto makeEdge = [&](b2Vec2 a, b2Vec2 b) {
            b2BodyDef bd;
            auto* body = m_world->CreateBody(&bd);
            b2EdgeShape edge;
            edge.SetTwoSided(a, b);
            b2FixtureDef fd;
            fd.shape       = &edge;
            fd.restitution = 0.45f;
            fd.friction    = 0.6f;
            body->CreateFixture(&fd);
        };

        makeEdge({0.f, 0.f},    {0.f, H + 4.f});
        makeEdge({W,   0.f},    {W,   H + 4.f});
        makeEdge({0.f, H + 2.f},{W,   H + 2.f});

        b2BodyDef bd;
        auto* gb = m_world->CreateBody(&bd);
        b2EdgeShape edge;
        edge.SetTwoSided({0.f, 0.f}, {W, 0.f});
        b2FixtureDef fd;
        fd.shape       = &edge;
        fd.restitution = 0.45f;
        fd.friction    = 0.6f;
        gb->CreateFixture(&fd);
        m_groundBody = gb;
    }

    void spawnOrbs() {
        auto winSize    = CCDirector::get()->getWinSize();
        auto gm         = GameManager::get();
        int  orbCount   = (int)Mod::get()->getSettingValue<int64_t>("orb-count");
        float orbScale  = (float)Mod::get()->getSettingValue<double>("orb-scale");
        int  cubeChance = (int)Mod::get()->getSettingValue<int64_t>("cube-chance");
        float W = winSize.width;
        float H = winSize.height;

        static const char* kFrames[] = {
            "ring_01_001.png",
            "ring_02_001.png",
            "ring_03_001.png",
            "ring_04_001.png",
            "ring_05_001.png",
            "ring_06_001.png",
            "ring_07_001.png",
            "ring_08_001.png",
            "ring_09_001.png",
        };

        for (int i = 0; i < orbCount; i++) {
            float radius = (20.f + (float)(rand() % 12)) * orbScale;

            auto spr = CCSprite::createWithSpriteFrameName(kFrames[rand() % 9]);
            if (spr) {
                float half = fmaxf(spr->getContentSize().width,
                                   spr->getContentSize().height) * 0.5f;
                if (half > 0.f) spr->setScale(radius / half);
            }

            float px = 40.f + (float)(rand() % (int)(W - 80.f));
            float py = H + (float)(rand() % 600 + 50);

            b2BodyDef bd;
            bd.type = b2_dynamicBody;
            bd.position.Set(px / PPM, py / PPM);
            bd.angle = (float)(rand() % 360) * b2_pi / 180.f;
            auto* body = m_world->CreateBody(&bd);

            b2CircleShape circle;
            circle.m_radius = radius / PPM;
            b2FixtureDef fd;
            fd.shape       = &circle;
            fd.density     = 1.0f;
            fd.restitution = 0.5f;
            fd.friction    = 0.8f;
            body->CreateFixture(&fd);

            body->ApplyLinearImpulse(
                b2Vec2((float)(rand() % 21 - 10) * 0.04f, 0.f),
                body->GetWorldCenter(), true
            );

            if (spr) {
                spr->setPosition(ccp(px, H - py));
                this->addChild(spr, 5);
            }

            m_orbs.push_back({body, spr, radius, false});
        }

        if ((rand() % 100) < cubeChance) {
            int cubeID = gm->activeIconForType(IconType::Cube);
            auto player = SimplePlayer::create(cubeID);
            if (player) {
                player->setColors(
                    GameManager::get()->colorForIdx(gm->getPlayerColor()),
                    GameManager::get()->colorForIdx(gm->getPlayerColor2())
                );
                float half = 28.f * orbScale;
                float naturalHalf = fmaxf(player->getContentSize().width,
                                          player->getContentSize().height) * 0.5f;
                if (naturalHalf > 0.f) player->setScale(half / naturalHalf);

                float px2 = 60.f + (float)(rand() % (int)(W - 120.f));
                float py2 = H + (float)(rand() % 300 + 300);

                b2BodyDef bd;
                bd.type = b2_dynamicBody;
                bd.position.Set(px2 / PPM, py2 / PPM);
                bd.angle = (float)(rand() % 360) * b2_pi / 180.f;
                auto* body = m_world->CreateBody(&bd);

                b2PolygonShape box;
                box.SetAsBox(half / PPM, half / PPM);
                b2FixtureDef fd;
                fd.shape       = &box;
                fd.density     = 1.0f;
                fd.restitution = 0.45f;
                fd.friction    = 0.7f;
                body->CreateFixture(&fd);

                player->setPosition(ccp(px2, H - py2));
                this->addChild(player, 6);
                m_orbs.push_back({body, player, half, true});
            }
        }
    }

    void update(float dt) override {
        if (dt > 0.05f) dt = 0.05f;

        auto winSize  = CCDirector::get()->getWinSize();
        bool noGround = Mod::get()->getSettingValue<bool>("no-ground");
        float H = winSize.height;

        if (noGround && m_groundBody) {
            m_world->DestroyBody(m_groundBody);
            m_groundBody = nullptr;
        }

        if (!noGround && !m_draining && m_groundBody) {
            m_drainTimer += dt;
            if (m_drainTimer > 6.0f) {
                m_draining = true;
                m_world->DestroyBody(m_groundBody);
                m_groundBody = nullptr;
            }
        }

        m_world->Step(dt, 8, 3);

        for (auto& o : m_orbs) {
            b2Vec2 pos = o.body->GetPosition();
            float  ang = o.body->GetAngle() * (180.f / b2_pi);
            float  screenY = H - pos.y * PPM;

            if (o.node) {
                o.node->setPosition(ccp(pos.x * PPM, screenY));
                o.node->setRotation(-ang);
            }
        }

        if (m_draining) {
            bool allGone = true;
            for (auto& o : m_orbs) {
                if (o.body->GetPosition().y * PPM > -300.f) {
                    allGone = false;
                    break;
                }
            }
            if (allGone) {
                m_draining   = false;
                m_drainTimer = 0.f;
                this->resetSim();
            }
        }

        if (noGround) {
            for (auto& o : m_orbs) {
                if (o.body->GetPosition().y * PPM < -100.f) {
                    float newX = (40.f + (float)(rand() % (int)(winSize.width - 80.f))) / PPM;
                    float newY = (H + (float)(rand() % 200 + 50)) / PPM;
                    o.body->SetTransform(b2Vec2(newX, newY), o.body->GetAngle());
                    o.body->SetLinearVelocity(b2Vec2(
                        (float)(rand() % 200 - 100) * 0.01f,
                        (float)(rand() % 100 + 20) * 0.01f
                    ));
                }
            }
        }
    }

    void resetSim() {
        for (auto& o : m_orbs) {
            if (o.node) o.node->removeFromParent();
            m_world->DestroyBody(o.body);
        }
        m_orbs.clear();

        b2BodyDef bd;
        auto* gb = m_world->CreateBody(&bd);
        auto winSize = CCDirector::get()->getWinSize();
        float W = winSize.width / PPM;
        b2EdgeShape edge;
        edge.SetTwoSided({0.f, 0.f}, {W, 0.f});
        b2FixtureDef fd;
        fd.shape = &edge; fd.restitution = 0.45f; fd.friction = 0.6f;
        gb->CreateFixture(&fd);
        m_groundBody = gb;

        m_drainTimer = 0.f;
        this->spawnOrbs();
    }

    void ccTouchesBegan(CCSet*, CCEvent*) override {
        if (Mod::get()->getSettingValue<bool>("dismiss-on-click"))
            this->dismiss();
    }

    void keyBackClicked() override {
        this->dismiss();
    }

    void onAnyKey() {
        if (Mod::get()->getSettingValue<bool>("dismiss-on-click"))
            this->dismiss();
    }

    void dismiss() {
        s_active = false;
        this->runAction(CCSequence::create(
            CCFadeOut::create(0.25f),
            CCCallFunc::create(this, callfunc_selector(OrbitScreensaverLayer::removeSelf)),
            nullptr
        ));
    }

    void removeSelf() {
        this->removeFromParentAndCleanup(true);
    }
};

bool OrbitScreensaverLayer::s_active = false;

static void resetIdle() {
    s_idleSeconds = 0.f;
}

static void tryShowScreensaver() {
    if (OrbitScreensaverLayer::s_active) return;
    if (!Mod::get()->getSettingValue<bool>("enabled")) return;

    auto scene = CCDirector::get()->getRunningScene();
    if (!scene) return;

    auto firstChild = scene->getChildByType<CCLayer>(0);
    if (firstChild) {
        std::string name = typeid(*firstChild).name();
        if (name.find("PlayLayer") != std::string::npos ||
            name.find("LevelEditorLayer") != std::string::npos)
            return;
    }

    OrbitScreensaverLayer::s_active = true;
    auto layer = OrbitScreensaverLayer::create();
    if (layer) {
        layer->setOpacity(0);
        scene->addChild(layer, 9999);
        layer->runAction(CCFadeIn::create(0.4f));
    } else {
        OrbitScreensaverLayer::s_active = false;
    }
}

class $modify(CCDirector) {
    void drawScene() {
        CCDirector::drawScene();
        if (!Mod::get()->getSettingValue<bool>("enabled")) return;
        if (OrbitScreensaverLayer::s_active) return;

        float idleSec = (float)Mod::get()->getSettingValue<int64_t>("idle-time");
        s_idleSeconds += (float)this->getActualDeltaTime();

        if (s_idleSeconds >= idleSec) {
            s_idleSeconds = 0.f;
            tryShowScreensaver();
        }
    }
};

class $modify(CCTouchDispatcher) {
    void touchesBegan(CCSet* t, CCEvent* e) {
        resetIdle();
        CCTouchDispatcher::touchesBegan(t, e);
    }
    void touchesMoved(CCSet* t, CCEvent* e) {
        resetIdle();
        CCTouchDispatcher::touchesMoved(t, e);
    }
    void touchesEnded(CCSet* t, CCEvent* e) {
        resetIdle();
        CCTouchDispatcher::touchesEnded(t, e);
    }
};

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isDown, bool isRepeat, double ts) {
        resetIdle();
        if (OrbitScreensaverLayer::s_active && isDown && !isRepeat) {
            auto scene = CCDirector::get()->getRunningScene();
            if (scene) {
                for (auto child : CCArrayExt<CCNode*>(scene->getChildren())) {
                    if (auto ss = dynamic_cast<OrbitScreensaverLayer*>(child)) {
                        ss->onAnyKey();
                        break;
                    }
                }
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isDown, isRepeat, ts);
    }
};

class $modify(CCMouseDispatcher) {
    bool dispatchScrollMSG(float x, float y) {
        resetIdle();
        return CCMouseDispatcher::dispatchScrollMSG(x, y);
    }
};
