/*
* Copyright 2014  Martin Gräßlin <mgraesslin@kde.org>
* Copyright 2014  Hugo Pereira Da Costa <hugo.pereira@free.fr>
* Copyright 2018  Vlad Zahorodnii <vlad.zahorodnii@kde.org>
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License as
* published by the Free Software Foundation; either version 2 of
* the License or (at your option) version 3 or any later version
* accepted by the membership of KDE e.V. (or its successor approved
* by the membership of KDE e.V.), which shall act as a proxy
* defined in Section 14 of version 3 of the license.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "breezedecoration.h"

#include "breeze.h"
#include "breezesettingsprovider.h"
#include "config-breeze.h"
#include "config/breezeconfigwidget.h"

#include "breezebutton.h"
#include "breezesizegrip.h"

#include "breezeboxshadowrenderer.h"

#include <KDecoration2/DecoratedClient>
#include <KDecoration2/DecorationButtonGroup>
#include <KDecoration2/DecorationSettings>
#include <KDecoration2/DecorationShadow>

#include <KColorUtils>
#include <KSharedConfig>
#include <KPluginFactory>

#include <QPainter>
#include <QTimer>
#include <QPropertyAnimation>

#if BREEZE_HAVE_X11

#include <QX11Info>

#endif

#include <QAbstractAnimation>
#include <QDir>
#include <KWindowInfo>
#include <QPainterPath>

K_PLUGIN_FACTORY_WITH_JSON(
        BreezeDecoFactory,
        "breeze.json",
        registerPlugin<Breeze::Decoration>();
        registerPlugin<Breeze::Button>();
        registerPlugin<Breeze::ConfigWidget>();
)

namespace {
    struct ShadowParams {
        ShadowParams()
                :
                offset({0, 0}),
                radius(0),
                opacity(0) {}

        ShadowParams(const QPoint &offset, int radius, qreal opacity)
                : offset(offset), radius(radius), opacity(opacity) {}

        QPoint offset;
        int radius;
        qreal opacity;
    };

    struct CompositeShadowParams {
        CompositeShadowParams() = default;

        CompositeShadowParams(
                const QPoint &offset,
                const ShadowParams &shadow1,
                const ShadowParams &shadow2)
                : offset(offset), shadow1(shadow1), shadow2(shadow2) {}

        bool isNone() const {
            return qMax(shadow1.radius, shadow2.radius) == 0;
        }

        QPoint offset;
        ShadowParams shadow1;
        ShadowParams shadow2;
    };

    const CompositeShadowParams s_shadowParams[] = {
            // None
            CompositeShadowParams(),
            // Small
            CompositeShadowParams(
                    QPoint(0, 4),
                    ShadowParams(QPoint(0, 0), 16, 1),
                    ShadowParams(QPoint(0, -2), 8, 0.4)),
            // Medium
            CompositeShadowParams(
                    QPoint(0, 8),
                    ShadowParams(QPoint(0, 0), 32, 0.9),
                    ShadowParams(QPoint(0, -4), 16, 0.3)),
            // Large
            CompositeShadowParams(
                    QPoint(0, 12),
                    ShadowParams(QPoint(0, 0), 48, 0.8),
                    ShadowParams(QPoint(0, -6), 24, 0.2)),
            // Very large
            CompositeShadowParams(
                    QPoint(0, 16),
                    ShadowParams(QPoint(0, 0), 64, 0.7),
                    ShadowParams(QPoint(0, -8), 32, 0.1))
    };

    inline CompositeShadowParams lookupShadowParams(int size) {
        switch (size) {
            case Breeze::InternalSettings::ShadowNone:
                return s_shadowParams[0];
            case Breeze::InternalSettings::ShadowSmall:
                return s_shadowParams[1];
            case Breeze::InternalSettings::ShadowMedium:
                return s_shadowParams[2];
            case Breeze::InternalSettings::ShadowLarge:
                return s_shadowParams[3];
            case Breeze::InternalSettings::ShadowVeryLarge:
                return s_shadowParams[4];
            default:
                // Fallback to the Large size.
                return s_shadowParams[3];
        }
    }

    inline CompositeShadowParams lookupShadowParamsInactiveWindows(int size) {
        switch (size) {
            case Breeze::InternalSettings::ShadowNoneInactiveWindows:
                return s_shadowParams[0];
            case Breeze::InternalSettings::ShadowSmallInactiveWindows:
                return s_shadowParams[1];
            case Breeze::InternalSettings::ShadowMediumInactiveWindows:
                return s_shadowParams[2];
            case Breeze::InternalSettings::ShadowLargeInactiveWindows:
                return s_shadowParams[3];
            case Breeze::InternalSettings::ShadowVeryLargeInactiveWindows:
                return s_shadowParams[4];
            default:
                // Fallback to the Large size.
                return s_shadowParams[3];
        }
    }
}

namespace Breeze {

    using KDecoration2::ColorRole;
    using KDecoration2::ColorGroup;

    //________________________________________________________________
    static int g_sDecoCount = 0;
    static int g_shadowSizeEnum = InternalSettings::ShadowLarge;
    static int g_shadowStrength = 255;
    static QColor g_shadowColor = Qt::black;
    static QSharedPointer<KDecoration2::DecorationShadow> g_sShadow;
    static bool g_specificShadowsInactiveWindows = false;
    static int g_shadowSizeEnumInactiveWindows = InternalSettings::ShadowLarge;
    static int g_shadowStrengthInactiveWindows = 255;
    static QColor g_shadowColorInactiveWindows = Qt::black;

    //________________________________________________________________
    Decoration::Decoration(QObject *parent, const QVariantList &args)
            : KDecoration2::Decoration(parent, args)
            , m_animation(new QVariantAnimation(this)) {
        g_sDecoCount++;
    }

    //________________________________________________________________
    Decoration::~Decoration() {
        g_sDecoCount--;
        if (g_sDecoCount == 0) {
            // last deco destroyed, clean up shadow
            g_sShadow.clear();
        }

        deleteSizeGrip();

    }

    //________________________________________________________________
    void Decoration::setOpacity(qreal value) {
        if (m_opacity == value) return;
        m_opacity = value;
        update();

        if (m_sizeGrip) m_sizeGrip->update();
    }

    //________________________________________________________________
    QColor Decoration::titleBarColor() const {
        auto c = client().toStrongRef().data();

        if (isKonsoleWindow(c)) return m_KonsoleTitleBarColor;
        if (hideTitleBar()) return c->color(ColorGroup::Inactive, ColorRole::TitleBar);

        else if (m_animation->state() == QPropertyAnimation::Running) {
            return KColorUtils::mix(
                    c->color(ColorGroup::Inactive, ColorRole::TitleBar),
                    c->color(ColorGroup::Active, ColorRole::TitleBar),
                    m_opacity
            );
        } else {
            return c->color(c->isActive() ? ColorGroup::Active : ColorGroup::Inactive, ColorRole::TitleBar);
        }
    }

    //________________________________________________________________
    QColor Decoration::outlineColor() const {
        auto c = client().toStrongRef().data();
        if (!m_internalSettings->drawTitleBarSeparator()) return {};
        if (m_animation->state() == QPropertyAnimation::Running) {
            QColor color(c->palette().color(QPalette::Highlight));
            color.setAlpha(color.alpha() * m_opacity);
            return color;
        } else if (c->isActive()) {
            return c->palette().color(QPalette::Highlight);
        } else {
            return {};
        }
    }

    //________________________________________________________________
    QColor Decoration::fontColor() const {
        auto c = client().toStrongRef().data();

        if (m_animation->state() == QPropertyAnimation::Running) {
            if (isKonsoleWindow(c)) {
                return KColorUtils::mix(
                        m_KonsoleTitleBarTextColorInactive,
                        m_KonsoleTitleBarTextColorActive,
                        m_opacity);
            } else {
                return KColorUtils::mix(
                        c->color(ColorGroup::Inactive, ColorRole::Foreground),
                        c->color(ColorGroup::Active, ColorRole::Foreground),
                        m_opacity);
            }
        } else {
            if (isKonsoleWindow(c)) {
                return c->isActive() ? m_KonsoleTitleBarTextColorActive : m_KonsoleTitleBarTextColorInactive;
            } else {
                return c->color(c->isActive() ? ColorGroup::Active : ColorGroup::Inactive, ColorRole::Foreground);
            }
        }
    }

    //________________________________________________________________
    void Decoration::setButtonHovered(bool value) {
        if (m_buttonHovered == value) {
            return;
        }
        m_buttonHovered = value;
        emit buttonHoveredChanged();
    }

    //________________________________________________________________
    void Decoration::hoverMoveEvent(QHoverEvent *event) {
        if (objectName() != "applet-window-buttons") {
            const bool groupContains = m_leftButtons->geometry().contains(event->posF()) ||
                                       m_rightButtons->geometry().contains(event->posF());
            setButtonHovered(groupContains);
        }

        KDecoration2::Decoration::hoverMoveEvent(event);
    }

    //________________________________________________________________
    void Decoration::init() {
        auto c = client().toStrongRef().data();

        // active state change animation
        // It is important start and end value are of the same type, hence 0.0 and not just 0
        m_animation->setStartValue(0.0);
        m_animation->setEndValue(1.0);
        // Linear to have the same easing as Breeze animations
        m_animation->setEasingCurve(QEasingCurve::Linear);
        connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            setOpacity(value.toReal());
        });

        reconfigure();
        updateTitleBar();
        auto s = settings();
        connect(s.data(), &KDecoration2::DecorationSettings::borderSizeChanged, this, &Decoration::recalculateBorders);

        // a change in font might cause the borders to change
        connect(s.data(), &KDecoration2::DecorationSettings::fontChanged, this,
                &Decoration::recalculateBorders); // recalculateBorders();
        connect(s.data(), &KDecoration2::DecorationSettings::spacingChanged, this, &Decoration::recalculateBorders);

        // buttons
        connect(s.data(), &KDecoration2::DecorationSettings::spacingChanged, this,
                &Decoration::updateButtonsGeometryDelayed);
        connect(s.data(), &KDecoration2::DecorationSettings::decorationButtonsLeftChanged, this,
                &Decoration::updateButtonsGeometryDelayed);
        connect(s.data(), &KDecoration2::DecorationSettings::decorationButtonsRightChanged, this,
                &Decoration::updateButtonsGeometryDelayed);

        // full reconfiguration
        connect(s.data(), &KDecoration2::DecorationSettings::reconfigured, this, &Decoration::reconfigure);
        connect(s.data(), &KDecoration2::DecorationSettings::reconfigured, SettingsProvider::self(),
                &SettingsProvider::reconfigure, Qt::UniqueConnection);
        connect(s.data(), &KDecoration2::DecorationSettings::reconfigured, this,
                &Decoration::updateButtonsGeometryDelayed);

        connect(c, &KDecoration2::DecoratedClient::adjacentScreenEdgesChanged, this, &Decoration::recalculateBorders);
        connect(c, &KDecoration2::DecoratedClient::maximizedHorizontallyChanged, this, &Decoration::recalculateBorders);
        connect(c, &KDecoration2::DecoratedClient::maximizedVerticallyChanged, this, &Decoration::recalculateBorders);
        connect(c, &KDecoration2::DecoratedClient::shadedChanged, this, &Decoration::recalculateBorders);
        connect(c, &KDecoration2::DecoratedClient::captionChanged, this,
                [this]() {
                    // update the caption area
                    update(titleBar());
                }
        );

        connect(c, &KDecoration2::DecoratedClient::activeChanged, this, &Decoration::updateAnimationState);
        connect(c, &KDecoration2::DecoratedClient::activeChanged, this, &Decoration::createShadow);
        connect(c, &KDecoration2::DecoratedClient::widthChanged, this, &Decoration::updateTitleBar);
        connect(c, &KDecoration2::DecoratedClient::maximizedChanged, this, &Decoration::updateTitleBar);
        //connect(c, &KDecoration2::DecoratedClient::maximizedChanged, this, &Decoration::setOpaque);

        connect(c, &KDecoration2::DecoratedClient::widthChanged, this, &Decoration::updateButtonsGeometry);
        connect(c, &KDecoration2::DecoratedClient::maximizedChanged, this, &Decoration::updateButtonsGeometry);
        connect(c, &KDecoration2::DecoratedClient::adjacentScreenEdgesChanged, this,
                &Decoration::updateButtonsGeometry);
        connect(c, &KDecoration2::DecoratedClient::shadedChanged, this, &Decoration::updateButtonsGeometry);

        createButtons();
        createShadow();
    }

    //________________________________________________________________
    void Decoration::updateTitleBar() {
        auto s = settings();
        auto c = client().toStrongRef().data();
        const bool maximized = isMaximized();
        const int width = maximized ? c->width() : c->width() - 2 * s->largeSpacing() * Metrics::TitleBar_SideMargin;
        const int height = maximized ? borderTop() : borderTop() - s->smallSpacing() * Metrics::TitleBar_TopMargin;
        const int x = maximized ? 0 : s->largeSpacing() * Metrics::TitleBar_SideMargin;
        const int y = maximized ? 0 : s->smallSpacing() * Metrics::TitleBar_TopMargin;
        setTitleBar(QRect(x, y, width, height));
    }

    //________________________________________________________________
    void Decoration::updateAnimationState() {
        if (m_internalSettings->animationsEnabled()) {
            auto c = client().toStrongRef().data();
            m_animation->setDirection(c->isActive() ? QAbstractAnimation::Forward : QAbstractAnimation::Backward);
            if (m_animation->state() != QAbstractAnimation::Running) m_animation->start();
        } else {
            update();
        }
    }

    //________________________________________________________________
    void Decoration::updateShadow() {
        auto c = client().toStrongRef().data();

        if (!g_specificShadowsInactiveWindows || c->isActive())
            updateActiveShadow();
        else
            updateInactiveShadow();
    }

    //________________________________________________________________
    void Decoration::updateActiveShadow() {

        CompositeShadowParams params;
        params = lookupShadowParams(g_shadowSizeEnum);

        if (params.isNone()) {
            g_sShadow.clear();
            setShadow(g_sShadow);
            return;
        }

        auto withOpacity = [](const QColor &color, qreal opacity) -> QColor {
            QColor c(color);
            c.setAlphaF(opacity);
            return c;
        };

        const QSize boxSize = BoxShadowRenderer::calculateMinimumBoxSize(params.shadow1.radius)
                .expandedTo(BoxShadowRenderer::calculateMinimumBoxSize(params.shadow2.radius));

        BoxShadowRenderer shadowRenderer;
        shadowRenderer.setBorderRadius(m_internalSettings->cornerRadius() + 0.5);
        shadowRenderer.setBoxSize(boxSize);
        shadowRenderer.setDevicePixelRatio(1.0); // TODO: Create HiDPI shadows?

        const qreal strength = static_cast<qreal>(g_shadowStrength) / 255.0;
        shadowRenderer.addShadow(params.shadow1.offset, params.shadow1.radius,
                                 withOpacity(g_shadowColor, params.shadow1.opacity * strength));
        shadowRenderer.addShadow(params.shadow2.offset, params.shadow2.radius,
                                 withOpacity(g_shadowColor, params.shadow2.opacity * strength));

        QImage shadowTexture = shadowRenderer.render();

        QPainter painter(&shadowTexture);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRect outerRect = shadowTexture.rect();

        QRect boxRect(QPoint(0, 0), boxSize);
        boxRect.moveCenter(outerRect.center());

        // Mask out inner rect.
        const QMargins padding = QMargins(
                boxRect.left() - outerRect.left() - Metrics::Shadow_Overlap - params.offset.x(),
                boxRect.top() - outerRect.top() - Metrics::Shadow_Overlap - params.offset.y(),
                outerRect.right() - boxRect.right() - Metrics::Shadow_Overlap + params.offset.x(),
                outerRect.bottom() - boxRect.bottom() - Metrics::Shadow_Overlap + params.offset.y());
        const QRect innerRect = outerRect - padding;

        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.drawRoundedRect(
                innerRect,
                m_internalSettings->cornerRadius() + 0.5,
                m_internalSettings->cornerRadius() + 0.5);

        // Draw outline.
        painter.setPen(withOpacity(g_shadowColor, 0.2 * strength));
        painter.setBrush(Qt::NoBrush);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.drawRoundedRect(
                innerRect,
                m_internalSettings->cornerRadius() - 0.5,
                m_internalSettings->cornerRadius() - 0.5);

        painter.end();

        g_sShadow = QSharedPointer<KDecoration2::DecorationShadow>::create();
        g_sShadow->setPadding(padding);
        g_sShadow->setInnerShadowRect(QRect(outerRect.center(), QSize(1, 1)));
        g_sShadow->setShadow(shadowTexture);

        setShadow(g_sShadow);
    }

    //________________________________________________________________
    void Decoration::updateInactiveShadow() {

        CompositeShadowParams params;
        params = lookupShadowParamsInactiveWindows(g_shadowSizeEnumInactiveWindows);

        if (params.isNone()) {
            g_sShadow.clear();
            setShadow(g_sShadow);
            return;
        }

        auto withOpacity = [](const QColor &color, qreal opacity) -> QColor {
            QColor c(color);
            c.setAlphaF(opacity);
            return c;
        };

        const QSize boxSize = BoxShadowRenderer::calculateMinimumBoxSize(params.shadow1.radius)
                .expandedTo(BoxShadowRenderer::calculateMinimumBoxSize(params.shadow2.radius));

        BoxShadowRenderer shadowRenderer;
        shadowRenderer.setBorderRadius(m_internalSettings->cornerRadius() + 0.5);
        shadowRenderer.setBoxSize(boxSize);
        shadowRenderer.setDevicePixelRatio(1.0); // TODO: Create HiDPI shadows?

        const qreal strength = static_cast<qreal>(g_shadowStrengthInactiveWindows) / 255.0;
        shadowRenderer.addShadow(params.shadow1.offset, params.shadow1.radius,
                                 withOpacity(g_shadowColorInactiveWindows, params.shadow1.opacity * strength));
        shadowRenderer.addShadow(params.shadow2.offset, params.shadow2.radius,
                                 withOpacity(g_shadowColorInactiveWindows, params.shadow2.opacity * strength));

        QImage shadowTexture = shadowRenderer.render();

        QPainter painter(&shadowTexture);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRect outerRect = shadowTexture.rect();

        QRect boxRect(QPoint(0, 0), boxSize);
        boxRect.moveCenter(outerRect.center());

        // Mask out inner rect.
        const QMargins padding = QMargins(
                boxRect.left() - outerRect.left() - Metrics::Shadow_Overlap - params.offset.x(),
                boxRect.top() - outerRect.top() - Metrics::Shadow_Overlap - params.offset.y(),
                outerRect.right() - boxRect.right() - Metrics::Shadow_Overlap + params.offset.x(),
                outerRect.bottom() - boxRect.bottom() - Metrics::Shadow_Overlap + params.offset.y());
        const QRect innerRect = outerRect - padding;

        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.drawRoundedRect(
                innerRect,
                m_internalSettings->cornerRadius() + 0.5,
                m_internalSettings->cornerRadius() + 0.5);

        // Draw outline.
        painter.setPen(withOpacity(g_shadowColorInactiveWindows, 0.2 * strength));
        painter.setBrush(Qt::NoBrush);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.drawRoundedRect(
                innerRect,
                m_internalSettings->cornerRadius() - 0.5,
                m_internalSettings->cornerRadius() - 0.5);

        painter.end();

        g_sShadow = QSharedPointer<KDecoration2::DecorationShadow>::create();
        g_sShadow->setPadding(padding);
        g_sShadow->setInnerShadowRect(QRect(outerRect.center(), QSize(1, 1)));
        g_sShadow->setShadow(shadowTexture);

        setShadow(g_sShadow);
    }

    //________________________________________________________________
    void Decoration::updateSizeGripVisibility() {
        auto c = client().toStrongRef().data();
        if (m_sizeGrip) {
            m_sizeGrip->setVisible(c->isResizeable() && !isMaximized() && !c->isShaded());
        }
    }

    //________________________________________________________________
    int Decoration::borderSize(bool bottom) const {
        if (bottom && isMaximized())
            return 0;

        const int baseSize = settings()->smallSpacing();
        if (m_internalSettings && (m_internalSettings->mask() & BorderSize)) {
            switch (m_internalSettings->borderSize()) {
                case InternalSettings::BorderNone:
                    return 0;
                case InternalSettings::BorderNoSides:
                    return bottom ? qMax(4, baseSize) : 0;
                default:
                case InternalSettings::BorderTiny:
                    return 1;
                case InternalSettings::BorderNormal:
                    return bottom ? qMax(4, baseSize) : baseSize;
                case InternalSettings::BorderLarge:
                    return baseSize * 2;
                case InternalSettings::BorderVeryLarge:
                    return baseSize * 3;
                case InternalSettings::BorderHuge:
                    return baseSize * 4;
                case InternalSettings::BorderVeryHuge:
                    return baseSize * 5;
                case InternalSettings::BorderOversized:
                    return baseSize * 6;
            }

        } else switch (settings()->borderSize()) {
            case KDecoration2::BorderSize::None:
                return 0;
            case KDecoration2::BorderSize::NoSides:
                return bottom ? baseSize * 6 : 0;
            default:
            case KDecoration2::BorderSize::Tiny:
                return 1;
            case KDecoration2::BorderSize::Normal:
                return bottom ? qMax(4, baseSize) : baseSize;
            case KDecoration2::BorderSize::Large:
                return baseSize * 2;
            case KDecoration2::BorderSize::VeryLarge:
                return baseSize * 3;
            case KDecoration2::BorderSize::Huge:
                return baseSize * 4;
            case KDecoration2::BorderSize::VeryHuge:
                return baseSize * 5;
            case KDecoration2::BorderSize::Oversized:
                return baseSize * 6;
        }
    }

    //________________________________________________________________
    void Decoration::reconfigure() {
        m_internalSettings = SettingsProvider::self()->internalSettings(this);

        // animation
        m_animation->setDuration(m_internalSettings->animationsDuration());

        // borders
        recalculateBorders();

        // shadow
        createShadow();

        // konsole title bar color and transparency
        readKonsoleProfileColor();

        // size grip
        if (borderSize() <= 1 && m_internalSettings->drawSizeGrip()) createSizeGrip();
        else deleteSizeGrip();
    }

    //________________________________________________________________
    void Decoration::recalculateBorders() {
        auto s = settings();

        // left, right and bottom borders
        const int left = isLeftEdge() ? 0 : borderSize();
        const int right = isRightEdge() ? 0 : borderSize();
        const int bottom = borderSize(true);

        int top = 0;
        if (hideTitleBar()) top = bottom;
        else {

            QFontMetrics fm(s->font());
            top += qMax(fm.height(), buttonHeight());

            // padding below
            // extra pixel is used for the active window outline
            const int baseSize = s->smallSpacing();
            top += baseSize * Metrics::TitleBar_BottomMargin + m_internalSettings->buttonPadding(); // + 1;

            // padding above
            top += baseSize * TitleBar_TopMargin + m_internalSettings->buttonPadding();

        }

        setBorders(QMargins(left, top, right, bottom));

        // extended sizes
        const int extSize = s->largeSpacing();
        int extHSides = 0;
        int extVSides = 0;
        if (borderSize() <= 1) {
            if (!isMaximizedHorizontally()) extHSides = extSize;
            if (!isMaximizedVertically()) extVSides = extSize;
        }

        setResizeOnlyBorders(QMargins(extHSides, extVSides, extHSides, extVSides));
    }

    //________________________________________________________________
    void Decoration::createButtons() {
        m_leftButtons = new KDecoration2::DecorationButtonGroup(KDecoration2::DecorationButtonGroup::Position::Left,
                                                                this, &Button::create);
        m_rightButtons = new KDecoration2::DecorationButtonGroup(KDecoration2::DecorationButtonGroup::Position::Right,
                                                                 this, &Button::create);
        updateButtonsGeometry();
    }

    void Decoration::readKonsoleProfileColor() {
        m_KonsoleTitleBarColorValid = false;

        const KConfig konsoleConfig("konsolerc");
        const QString defaultProfileFile = konsoleConfig.group("Desktop Entry").readEntry("DefaultProfile", QString());

        // Konsole config profile path
        const QString configLocation(QDir::homePath() + "/.local/share/konsole/");
        const QString shellProfileLocation(configLocation + defaultProfileFile);

        if (!QFile::exists(shellProfileLocation)) {
            return;
        }

        const KConfig configProfile(shellProfileLocation, KConfig::NoGlobals);

        const QString colorFileLocation =
                configLocation + configProfile.group("Appearance").readEntry("ColorScheme", QString()) + ".colorscheme";

        if (!QFile::exists(colorFileLocation)) {
            return;
        }

        const KConfig configColor(colorFileLocation, KConfig::NoGlobals);
        const QStringList backgroundRGB = configColor.group("Background").readEntry("Color").split(',');

        if (backgroundRGB.size() != 3) {
            return;
        }

        m_KonsoleTitleBarColor.setRed(backgroundRGB[0].toInt());
        m_KonsoleTitleBarColor.setGreen(backgroundRGB[1].toInt());
        m_KonsoleTitleBarColor.setBlue(backgroundRGB[2].toInt());
        m_KonsoleTitleBarColor.setAlphaF(configColor.group("General").readEntry("Opacity").toDouble());

        // Text color
        const QStringList foregroundRGB = configColor.group("Foreground").readEntry("Color").split(',');

        if (foregroundRGB.size() != 3) {
            return;
        }

        m_KonsoleTitleBarTextColorActive.setRed(foregroundRGB[0].toInt());
        m_KonsoleTitleBarTextColorActive.setGreen(foregroundRGB[1].toInt());
        m_KonsoleTitleBarTextColorActive.setBlue(foregroundRGB[2].toInt());

        m_KonsoleTitleBarTextColorInactive = m_KonsoleTitleBarTextColorActive;
        m_KonsoleTitleBarTextColorInactive.setAlphaF(0.5);

        m_KonsoleTitleBarColorValid = true;
    }

    bool Decoration::isKonsoleWindow(KDecoration2::DecoratedClient *dc) const {
        if (!m_KonsoleTitleBarColorValid) {
            return false;
        }

        KWindowInfo info(dc->windowId(), NET::Properties(), NET::WM2WindowClass | NET::WM2WindowRole);

        return info.valid() &&
               info.windowClassClass() == QByteArray("konsole") &&
               info.windowRole().startsWith("MainWindow");
    }

    //________________________________________________________________
    void Decoration::updateButtonsGeometryDelayed() const {
        QTimer::singleShot(0, this, &Decoration::updateButtonsGeometry);
    }

    //________________________________________________________________
    void Decoration::updateButtonsGeometry() {
        const auto s = settings();

        // adjust button position
        const int bHeight = captionHeight() + (isTopEdge() ? s->smallSpacing() * Metrics::TitleBar_TopMargin : 0);
        const int bWidth = buttonHeight();
        const int verticalOffset = (isTopEdge() ? s->smallSpacing() * Metrics::TitleBar_TopMargin : 0) +
                                   (captionHeight() - buttonHeight()) / 2;

        foreach(const QPointer<KDecoration2::DecorationButton> &button, m_leftButtons->buttons() + m_rightButtons->buttons()) {
            button.data()->setGeometry(QRectF(QPoint(0, 0), QSizeF(bWidth, bHeight)));
            dynamic_cast<Button *>( button.data())->setOffset(QPointF(0, verticalOffset));
            dynamic_cast<Button *>( button.data())->setIconSize(QSize(bWidth, bWidth));
        }

        // left buttons
        if (!m_leftButtons->buttons().isEmpty()) {
            // spacing (use our own spacing instead of s->smallSpacing()*Metrics::TitleBar_ButtonSpacing)
            m_leftButtons->setSpacing(m_internalSettings->buttonSpacing());

            // padding
            const int vPadding = isTopEdge() ? 0 : s->smallSpacing() * Metrics::TitleBar_TopMargin;
            // const int hPadding = s->smallSpacing()*Metrics::TitleBar_SideMargin;
            const int hPadding = m_internalSettings->buttonPadding() + m_internalSettings->buttonHOffset();
            if (isLeftEdge()) {
                // add offsets on the side buttons, to preserve padding, but satisfy Fitts law
                auto button = dynamic_cast<Button *>( m_leftButtons->buttons().front().data());
                button->setGeometry(QRectF(QPoint(0, 0), QSizeF(bWidth + hPadding, bHeight)));
                button->setFlag(Button::FlagFirstInList);
                button->setHorizontalOffset(hPadding);

                m_leftButtons->setPos(QPointF(0, vPadding));

            } else m_leftButtons->setPos(QPointF(hPadding + borderLeft(), vPadding));

        }

        // right buttons
        if (!m_rightButtons->buttons().isEmpty()) {

            // spacing (use our own spacing instead of s->smallSpacing()*Metrics::TitleBar_ButtonSpacing)
            m_rightButtons->setSpacing(m_internalSettings->buttonSpacing());

            // padding
            const int vPadding = isTopEdge() ? 0 : s->smallSpacing() * Metrics::TitleBar_TopMargin;
            // const int hPadding = s->smallSpacing()*Metrics::TitleBar_SideMargin;
            const int hPadding = m_internalSettings->buttonPadding() + m_internalSettings->buttonHOffset();
            if (isRightEdge()) {

                auto button = dynamic_cast<Button *>( m_rightButtons->buttons().back().data());
                button->setGeometry(QRectF(QPoint(0, 0), QSizeF(bWidth + hPadding, bHeight)));
                button->setFlag(Button::FlagLastInList);

                m_rightButtons->setPos(QPointF(size().width() - m_rightButtons->geometry().width(), vPadding));

            } else
                m_rightButtons->setPos(
                        QPointF(size().width() - m_rightButtons->geometry().width() - hPadding - borderRight(),
                                vPadding));

        }

        update();

    }

    //________________________________________________________________
    void Decoration::paint(QPainter *painter, const QRect &repaintRegion) {
        // TODO: optimize based on repaintRegion
        auto c = client().toStrongRef().data();
        auto s = settings();

        QColor titleBarColor = this->titleBarColor();

        // paint background
        if (!c->isShaded()) {
            painter->fillRect(rect(), Qt::transparent);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setBrush(titleBarColor);
            // clip away the top part
            // if( !hideTitleBar() ) painter->setClipRect(0, borderTop(), size().width(), size().height() - borderTop(), Qt::IntersectClip)

            QPen borderPen(titleBarColor.darker(125));
            painter->setPen(borderPen);
            if (s->isAlphaChannelSupported() && !isMaximized()) {
                painter->drawRoundedRect(rect(), m_internalSettings->cornerRadius(),m_internalSettings->cornerRadius());
            } else {
                painter->drawRect(rect());
            }

            painter->restore();
        }

        paintTitleBar(painter, repaintRegion);

        if (hasBorders()) {
            painter->save();
//            painter->setRenderHint(QPainter::Antialiasing, false);
            painter->setBrush(Qt::NoBrush);

            QPen borderPen(titleBarColor);
            painter->setPen(borderPen);
            if (s->isAlphaChannelSupported() && !isMaximized()) {
                painter->drawRoundedRect(rect(), m_internalSettings->cornerRadius(),m_internalSettings->cornerRadius());
            } else {
                painter->drawRect(rect());
            }

            painter->restore();
        }

    }

    //________________________________________________________________
    void Decoration::paintTitleBar(QPainter *painter, const QRect &repaintRegion) {
        const auto c = client().toStrongRef().data();

        const QColor matchedTitleBarColor(c->palette().color(QPalette::Window));

        const QRect titleRect(QPoint(0, 0), QSize(size().width(), borderTop()));
        if (!titleRect.intersects(repaintRegion)) return;

        QColor outlineColor = this->outlineColor();

        painter->save();
        painter->setPen(Qt::NoPen);

        // render a linear gradient on title area
        if (drawBackgroundGradient() && !isKonsoleWindow(c)) {
            const QColor titleBarColor = (matchColorForTitleBar() ? matchedTitleBarColor : this->titleBarColor());

            QLinearGradient gradient(0, 0, 0, titleRect.height());
            int b = m_internalSettings->gradientOverride() > -1 ? m_internalSettings->gradientOverride()
                                                                : m_internalSettings->backgroundGradientIntensity();
            if (!c->isActive()) b *= 0.5;
            b = qBound(0, b, 100);
            gradient.setColorAt(0.0, titleBarColor.lighter(100 + b));
            gradient.setColorAt(1.0, titleBarColor);
            painter->setBrush(gradient);
        } else if (!isKonsoleWindow(c)) {
            const QColor titleBarColor = (matchColorForTitleBar() ? matchedTitleBarColor : this->titleBarColor());
            painter->setBrush(titleBarColor);
        } else {
            const QColor titleBarColor = this->titleBarColor();
            painter->setBrush(titleBarColor);
        }

        auto s = settings();
        if (!s->isAlphaChannelSupported() || isMaximized()) {
            painter->drawRect(titleRect);
        } else {
            painter->setClipRect(titleRect, Qt::IntersectClip);
            // the rect is made a little bit larger to be able to clip away the rounded corners at the bottom and sides
            const QRect rect = titleRect.adjusted(
                    isLeftEdge() ? -m_internalSettings->cornerRadius() : 0,
                    isTopEdge() ? -m_internalSettings->cornerRadius() : 0,
                    isRightEdge() ? m_internalSettings->cornerRadius() : 0,
                    m_internalSettings->cornerRadius());

            painter->drawRoundedRect(rect, m_internalSettings->cornerRadius(), m_internalSettings->cornerRadius());
        }

        if (!c->isShaded() && !hideTitleBar() && outlineColor.isValid()) {
            // outline
            painter->setRenderHint(QPainter::Antialiasing, false);
            painter->setBrush(Qt::NoBrush);
            QPen pen(outlineColor);
            pen.setWidth(1);
            painter->setPen(pen);
            painter->drawLine(titleRect.bottomLeft() + QPoint(borderSize(), 0),
                              titleRect.bottomRight() - QPoint(borderSize(), 0));
        }

        painter->restore();

        if (!hideTitleBar()) {
            // draw caption
            painter->setFont(s->font());
            painter->setPen(fontColor());
            const auto cR = captionRect();
            const QString caption = painter->fontMetrics().elidedText(c->caption(), Qt::ElideMiddle, cR.first.width());
            painter->drawText(cR.first, cR.second | Qt::TextSingleLine, caption);

            // draw all buttons
            m_leftButtons->paint(painter, repaintRegion);
            m_rightButtons->paint(painter, repaintRegion);
        }
    }

    //________________________________________________________________
    int Decoration::buttonHeight() const {
        const int baseSize = settings()->gridUnit();
        switch (m_internalSettings->buttonSize()) {
            case InternalSettings::ButtonTiny:
                return baseSize;
            case InternalSettings::ButtonSmall:
                return baseSize * 1.5;
            default:
            case InternalSettings::ButtonDefault:
                return baseSize * 2;
            case InternalSettings::ButtonLarge:
                return baseSize * 2.5;
            case InternalSettings::ButtonVeryLarge:
                return baseSize * 3.5;
        }

    }

    //________________________________________________________________
    int Decoration::captionHeight() const {
        return hideTitleBar() ? borderTop() : borderTop() - settings()->smallSpacing() *
                                                            (Metrics::TitleBar_BottomMargin +
                                                             Metrics::TitleBar_TopMargin);
    }

    //________________________________________________________________
    QPair<QRect, Qt::Alignment> Decoration::captionRect() const {
        if (hideTitleBar()) return qMakePair(QRect(), Qt::AlignCenter);
        else {

            auto c = client().toStrongRef().data();
            const int leftOffset = m_leftButtons->buttons().isEmpty() ?
                                   Metrics::TitleBar_SideMargin * settings()->smallSpacing() :
                                   m_leftButtons->geometry().x() + m_leftButtons->geometry().width() +
                                   Metrics::TitleBar_SideMargin * settings()->smallSpacing();

            const int rightOffset = m_rightButtons->buttons().isEmpty() ?
                                    Metrics::TitleBar_SideMargin * settings()->smallSpacing() :
                                    size().width() - m_rightButtons->geometry().x() +
                                    Metrics::TitleBar_SideMargin * settings()->smallSpacing();

            const int yOffset = settings()->smallSpacing() * Metrics::TitleBar_TopMargin;
            const QRect maxRect(leftOffset, yOffset, size().width() - leftOffset - rightOffset, captionHeight());

            switch (m_internalSettings->titleAlignment()) {
                case InternalSettings::AlignLeft:
                    return qMakePair(maxRect, Qt::AlignVCenter | Qt::AlignLeft);

                case InternalSettings::AlignRight:
                    return qMakePair(maxRect, Qt::AlignVCenter | Qt::AlignRight);

                case InternalSettings::AlignCenter:
                    return qMakePair(maxRect, Qt::AlignCenter);

                default:
                case InternalSettings::AlignCenterFullWidth: {

                    // full caption rect
                    const QRect fullRect = QRect(0, yOffset, size().width(), captionHeight());
                    QRect boundingRect(settings()->fontMetrics().boundingRect(c->caption()).toRect());

                    // text bounding rect
                    boundingRect.setTop(yOffset);
                    boundingRect.setHeight(captionHeight());
                    boundingRect.moveLeft((size().width() - boundingRect.width()) / 2);

                    if (boundingRect.left() < leftOffset) return qMakePair(maxRect, Qt::AlignVCenter | Qt::AlignLeft);
                    else if (boundingRect.right() > size().width() - rightOffset)
                        return qMakePair(maxRect, Qt::AlignVCenter | Qt::AlignRight);
                    else return qMakePair(fullRect, Qt::AlignCenter);

                }
            }
        }
    }

    //________________________________________________________________
    void Decoration::createShadow() {
        if (!g_sShadow) {
            g_shadowSizeEnum = m_internalSettings->shadowSize();
            g_shadowStrength = m_internalSettings->shadowStrength();
            g_shadowColor = m_internalSettings->shadowColor();
            g_specificShadowsInactiveWindows = m_internalSettings->specificShadowsInactiveWindows();
            g_shadowSizeEnumInactiveWindows = m_internalSettings->shadowSizeInactiveWindows();
            g_shadowStrengthInactiveWindows = m_internalSettings->shadowStrengthInactiveWindows();
            g_shadowColorInactiveWindows = m_internalSettings->shadowColorInactiveWindows();

            updateShadow();
        } else if (g_shadowSizeEnum != m_internalSettings->shadowSize()
                   || g_shadowStrength != m_internalSettings->shadowStrength()
                   || g_shadowColor != m_internalSettings->shadowColor()) {
            g_shadowSizeEnum = m_internalSettings->shadowSize();
            g_shadowStrength = m_internalSettings->shadowStrength();
            g_shadowColor = m_internalSettings->shadowColor();

            updateActiveShadow();
        } else if (g_specificShadowsInactiveWindows != m_internalSettings->specificShadowsInactiveWindows()
                   || g_shadowSizeEnumInactiveWindows != m_internalSettings->shadowSizeInactiveWindows()
                   || g_shadowStrengthInactiveWindows != m_internalSettings->shadowStrengthInactiveWindows()
                   || g_shadowColorInactiveWindows != m_internalSettings->shadowColorInactiveWindows()) {
            g_specificShadowsInactiveWindows = m_internalSettings->specificShadowsInactiveWindows();
            g_shadowSizeEnumInactiveWindows = m_internalSettings->shadowSizeInactiveWindows();
            g_shadowStrengthInactiveWindows = m_internalSettings->shadowStrengthInactiveWindows();
            g_shadowColorInactiveWindows = m_internalSettings->shadowColorInactiveWindows();

            updateInactiveShadow();
        } else
            updateShadow();
    }

    //_________________________________________________________________
    void Decoration::createSizeGrip() {

        // do nothing if size grip already exist
        if (m_sizeGrip) return;

#if BREEZE_HAVE_X11
        if (!QX11Info::isPlatformX11()) return;

        // access client
        auto c = client().toStrongRef().data();
        if (!c) return;

        if (c->windowId() != 0) {
            m_sizeGrip = new SizeGrip(this);
            connect(c, &KDecoration2::DecoratedClient::maximizedChanged, this, &Decoration::updateSizeGripVisibility);
            connect(c, &KDecoration2::DecoratedClient::shadedChanged, this, &Decoration::updateSizeGripVisibility);
            connect(c, &KDecoration2::DecoratedClient::resizeableChanged, this, &Decoration::updateSizeGripVisibility);
        }
#endif

    }

    //_________________________________________________________________
    void Decoration::deleteSizeGrip() {
        if (m_sizeGrip) {
            m_sizeGrip->deleteLater();
            m_sizeGrip = nullptr;
        }
    }

} // namespace


#include "breezedecoration.moc"
