#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCMouseDispatcher.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/SimplePlayer.hpp>

using namespace geode::prelude;


struct PhysOrb {
    CCNode*  node     = nullptr;
    CCSize   size     = {40, 40}; 
    bool     isCircle = true;

    CCPoint  pos      = {0, 0};
    CCPoint  vel      = {0, 0};
    float    angle    = 0;
    float    angVel   = 0;
    float    radius   = 20;
};



static const char* kOrbFrames[] = {
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
static const int kNumOrbFrames = sizeof(kOrbFrames) / sizeof(kOrbFrames[0]);


class OrbitScreensaverLayer : public CCLayerColor {
public:
    std::vector<PhysOrb> m_orbs;
    float  m_gravity   = 600.f;
    float  m_restitution = 0.55f;
    float  m_friction    = 0.85f; 

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
        auto bgCol = Mod::get()->getSettingValue<ccColor4B>("bg-color");
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

        this->spawnOrbs();
        this->scheduleUpdate();
        return true;
    }
    void spawnOrbs() {
        auto winSize = CCDirector::get()->getWinSize();
        auto gm      = GameManager::get();

        int  orbCount  = (int)Mod::get()->getSettingValue<int64_t>("orb-count");
        float orbScale = (float)Mod::get()->getSettingValue<double>("orb-scale");
        int  cubeChance = (int)Mod::get()->getSettingValue<int64_t>("cube-chance");
        float speedMul  = (float)Mod::get()->getSettingValue<double>("speed");

        m_gravity = 600.f * speedMul;
        for (int i = 0; i < orbCount; ++i) {
            const char* frame = kOrbFrames[rand() % kNumOrbFrames];
            auto spr = CCSprite::createWithSpriteFrameName(frame);

            float baseRadius = 20.f * orbScale;
            if (spr) {
                float naturalHalf = fmaxf(spr->getContentSize().width,
                                          spr->getContentSize().height) * 0.5f;
                if (naturalHalf > 0.f)
                    spr->setScale((baseRadius / naturalHalf));
            } else {
                spr = CCSprite::create();
            }

            float x = 40.f + (float)(rand() % (int)(winSize.width  - 80.f));
            float y = -(float)(rand() % 400 + 50);   // start above screen

            PhysOrb orb;
            orb.node     = spr;
            orb.isCircle = true;
            orb.radius   = baseRadius;
            orb.size     = CCSizeMake(baseRadius * 2, baseRadius * 2);
            orb.pos      = ccp(x, y);
            orb.vel      = ccp((float)(rand() % 200 - 100),
                               (float)(rand() % 100 + 50));
            orb.angle    = (float)(rand() % 360);
            orb.angVel   = (float)(rand() % 300 - 150);

            if (spr) {
                spr->setPosition(ccp(x, y));
                this->addChild(spr, 5);
            }
            m_orbs.push_back(orb);
        }
        if ((rand() % 100) < cubeChance) {
            int cubeID  = gm->activeIconForType(IconType::Cube);
            auto player = SimplePlayer::create(cubeID);
            if (player) {
                player->setColors(
                    GameManager::get()->colorForIdx(gm->getPlayerColor()),
                    GameManager::get()->colorForIdx(gm->getPlayerColor2())
                );
                float cubeHalf = 28.f * orbScale;
                float naturalHalf = fmaxf(player->getContentSize().width,
                                          player->getContentSize().height) * 0.5f;
                if (naturalHalf > 0.f)
                    player->setScale((cubeHalf / naturalHalf));

                float x2 = 60.f + (float)(rand() % (int)(winSize.width - 120.f));
                float y2 = -(float)(rand() % 200 + 300);

                PhysOrb orb;
                orb.node     = player;
                orb.isCircle = false;
                orb.radius   = cubeHalf;
                orb.size     = CCSizeMake(cubeHalf * 2, cubeHalf * 2);
                orb.pos      = ccp(x2, y2);
                orb.vel      = ccp((float)(rand() % 160 - 80),
                                   (float)(rand() % 80 + 30));
                orb.angle    = (float)(rand() % 360);
                orb.angVel   = (float)(rand() % 200 - 100);

                player->setPosition(ccp(x2, y2));
                this->addChild(player, 6);
                m_orbs.push_back(orb);
            }
        }
    }
    void update(float dt) override {
        if (dt > 0.05f) dt = 0.05f;

        auto winSize = CCDirector::get()->getWinSize();
        bool noGround = Mod::get()->getSettingValue<bool>("no-ground");

        float W = winSize.width;
        float H = winSize.height;

        for (auto& orb : m_orbs) {
            orb.vel.y += m_gravity * dt;
          
            orb.pos.x += orb.vel.x * dt;
            orb.pos.y += orb.vel.y * dt;

            orb.angle += orb.angVel * dt;

            float r = orb.radius;

            if (orb.pos.x - r < 0.f) {
                orb.pos.x = r;
                orb.vel.x = fabsf(orb.vel.x) * m_restitution;
                orb.vel.y *= m_friction;
                orb.angVel = -orb.angVel * 0.7f;
            }
            if (orb.pos.x + r > W) {
                orb.pos.x = W - r;
                orb.vel.x = -fabsf(orb.vel.x) * m_restitution;
                orb.vel.y *= m_friction;
                orb.angVel = -orb.angVel * 0.7f;
            }
            if (!noGround && orb.pos.y + r > H) {
                orb.pos.y = H - r;
                orb.vel.y = -fabsf(orb.vel.y) * m_restitution;
                orb.vel.x *= m_friction;
                orb.angVel *= 0.85f;
                if (fabsf(orb.vel.y) < 20.f) orb.vel.y = 0.f;
            }
            if (orb.pos.y - r < 0.f) {
                orb.pos.y = r;
                orb.vel.y = fabsf(orb.vel.y) * m_restitution;
            }

            if (noGround && orb.pos.y - r > H + 50.f) {
                orb.pos.x = 40.f + (float)(rand() % (int)(W - 80.f));
                orb.pos.y = -(float)(rand() % 100 + 40);
                orb.vel.x = (float)(rand() % 200 - 100);
                orb.vel.y = (float)(rand() % 100 + 20);
                orb.angVel = (float)(rand() % 300 - 150);
            }

            if (orb.node) {
                float screenY = H - orb.pos.y;
                orb.node->setPosition(ccp(orb.pos.x, screenY));
                orb.node->setRotation(orb.angle);
            }
        }
        for (size_t i = 0; i < m_orbs.size(); ++i) {
            for (size_t j = i + 1; j < m_orbs.size(); ++j) {
                auto& a = m_orbs[i];
                auto& b = m_orbs[j];

                float dx    = b.pos.x - a.pos.x;
                float dy    = b.pos.y - a.pos.y;
                float dist  = sqrtf(dx * dx + dy * dy);
                float minD  = a.radius + b.radius;

                if (dist < minD && dist > 0.001f) {
                    float nx = dx / dist;
                    float ny = dy / dist;

                    float overlap = (minD - dist) * 0.5f;
                    a.pos.x -= nx * overlap;
                    a.pos.y -= ny * overlap;
                    b.pos.x += nx * overlap;
                    b.pos.y += ny * overlap;

                    float rvn = (b.vel.x - a.vel.x) * nx +
                                (b.vel.y - a.vel.y) * ny;

                    if (rvn < 0.f) {
                        float imp = -(1.f + m_restitution) * rvn * 0.5f;
                        a.vel.x -= imp * nx;
                        a.vel.y -= imp * ny;
                        b.vel.x += imp * nx;
                        b.vel.y += imp * ny;

                        float spinTransfer = imp * 0.3f;
                        a.angVel -= spinTransfer * (rand() % 2 ? 1 : -1);
                        b.angVel += spinTransfer * (rand() % 2 ? 1 : -1);
                    }
                }
            }
        }
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
        this->runAction(CCSequence::create(
            CCFadeOut::create(0.25f),
            CCCallFunc::create(this, callfunc_selector(OrbitScreensaverLayer::removeSelf)),
            nullptr
        ));
        OrbitScreensaverLayer::s_active = false;
    }

    void removeSelf() {
        this->removeFromParentAndCleanup(true);
    }

    static bool s_active;
};

bool OrbitScreensaverLayer::s_active = false;

//  Idle tracker — hooks onto CCDirector::drawScene

static float s_idleTime      = 0.f;
static float s_lastInputTime = 0.f; 

static void resetIdle() {
    s_lastInputTime = CCDirector::get()->getTotalFrames() *
                      (float)CCDirector::get()->getAnimationInterval();
    s_idleTime = 0.f;
}

static void tryShowScreensaver() {
    if (OrbitScreensaverLayer::s_active) return;
    if (!Mod::get()->getSettingValue<bool>("enabled")) return;

    // Don't show inside PlayLayer (im playing)
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
        float dt      = (float)this->getActualDeltaTime();
        s_idleTime   += dt;

        if (s_idleTime >= idleSec) {
            s_idleTime = 0.f;
            tryShowScreensaver();
        }
    }
};

class $modify(CCTouchDispatcher) {
    void touchesBegan(CCSet* touches, CCEvent* ev) {
        resetIdle();
        if (OrbitScreensaverLayer::s_active) {
            CCTouchDispatcher::touchesBegan(touches, ev);
            return;
        }
        CCTouchDispatcher::touchesBegan(touches, ev);
    }

    void touchesMoved(CCSet* touches, CCEvent* ev) {
        resetIdle();
        CCTouchDispatcher::touchesMoved(touches, ev);
    }

    void touchesEnded(CCSet* touches, CCEvent* ev) {
        resetIdle();
        CCTouchDispatcher::touchesEnded(touches, ev);
    }
};

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isDown, bool isRepeat, double timestamp) {
        resetIdle();

        if (OrbitScreensaverLayer::s_active && isDown && !isRepeat) {
            auto scene = CCDirector::get()->getRunningScene();
            if (scene) {
                for (auto child : CCArrayExt<CCNode*>(scene->getChildren())) {
                    if (auto ssLayer = dynamic_cast<OrbitScreensaverLayer*>(child)) {
                        ssLayer->onAnyKey();
                        break;
                    }
                }
            }
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isDown, isRepeat, timestamp);
    }
};

class $modify(CCMouseDispatcher) {
    bool dispatchScrollMSG(float x, float y) {
        resetIdle();
        return CCMouseDispatcher::dispatchScrollMSG(x, y);
    }
};
