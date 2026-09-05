#include "tva_core.h"

#include <QtCore/QtGlobal>
#include <QtGui/QDoubleValidator>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

QString qstr(const std::string& value) {
    return QString::fromUtf8(value.c_str());
}

std::string utf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QPointF mousePosition(QMouseEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->localPos();
#endif
}

std::string formatCompact(double value) {
    std::ostringstream out;
    out << std::setprecision(12) << value;
    return out.str();
}

std::pair<double, double> paddedRange(double minValue, double maxValue) {
    if (!std::isfinite(minValue) || !std::isfinite(maxValue)) {
        return {-1.0, 1.0};
    }
    if (std::abs(maxValue - minValue) < 1e-12) {
        const double base = std::max(1.0, std::abs(minValue));
        return {minValue - base, maxValue + base};
    }
    const double pad = (maxValue - minValue) * 0.08;
    return {minValue - pad, maxValue + pad};
}

QColor colorForIndex(std::size_t index) {
    static const QColor colors[] = {
        QColor(34, 94, 168), QColor(226, 83, 63), QColor(38, 139, 92),
        QColor(139, 92, 246), QColor(196, 127, 0), QColor(15, 118, 110),
        QColor(190, 24, 93), QColor(17, 24, 39)
    };
    return colors[index % (sizeof(colors) / sizeof(colors[0]))];
}

struct Range {
    double minX{};
    double maxX{};
    double minY{};
    double maxY{};
    double stepX{};
    double stepY{};
};

double niceStep(double span, int targetCells = 6) {
    if (!std::isfinite(span) || span <= 0.0) {
        return 1.0;
    }
    const double raw = span / static_cast<double>(targetCells);
    const double power = std::pow(10.0, std::floor(std::log10(raw)));
    const double normalized = raw / power;
    double factor = 10.0;
    if (normalized <= 1.0) {
        factor = 1.0;
    } else if (normalized <= 2.0) {
        factor = 2.0;
    } else if (normalized <= 5.0) {
        factor = 5.0;
    }
    return factor * power;
}

Range makeRange(double minX, double maxX, double minY, double maxY, bool forceBookScale = false) {
    if (forceBookScale) {
        return {0.0, 6.0, 0.0, 12.0, 1.0, 2.0};
    }
    const auto xr = paddedRange(minX, maxX);
    const auto yr = paddedRange(minY, maxY);
    return {xr.first, xr.second, yr.first, yr.second,
        niceStep(xr.second - xr.first), niceStep(yr.second - yr.first)};
}

QString tickLabel(double value, double step) {
    const double normalized = std::abs(value) < std::abs(step) * 1e-6 ? 0.0 : value;
    int precision = 0;
    if (std::abs(step) < 1.0) {
        precision = std::abs(step) < 0.01 ? 3 : (std::abs(step) < 0.1 ? 2 : 1);
    }
    return QString::number(normalized, 'f', precision);
}

enum class XMode {
    Time,
    LogTime,
    ShiftedLogTime
};

class PlotWidget final : public QWidget {
public:
    explicit PlotWidget(QWidget* parent = nullptr)
        : QWidget(parent) {
        setMinimumSize(760, 900);
    }

    void setState(ExperimentInput* input, std::optional<FitResult>* fit) {
        input_ = input;
        fit_ = fit;
    }

    void setAddCallback(std::function<void(double, double)> onAddPoint) {
        onAddPoint_ = std::move(onAddPoint);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), Qt::white);

        if (!input_ || input_->curves.empty()) {
            painter.setPen(QColor(75, 85, 99));
            painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignLeft | Qt::AlignTop,
                QStringLiteral("Load or add creep curves."));
            return;
        }

        const QRectF rawTime = rawTimeRect();
        const QRectF rawLog = rawLogRect();
        const QRectF shifted = shiftedRect();
        const QRectF shifts = shiftsRect();

        painter.setPen(QColor(23, 32, 51));
        QFont titleFont = painter.font();
        titleFont.setBold(true);
        titleFont.setPointSize(11);
        painter.setFont(titleFont);
        painter.drawText(QRectF(18, 14, width() - 36, 24), Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("Experiment: %1").arg(qstr(input_->experimentId)));

        QFont infoFont = painter.font();
        infoFont.setBold(false);
        infoFont.setPointSize(9);
        painter.setFont(infoFont);
        const QString info = fit_ && *fit_
            ? QStringLiteral("C1=%1    C2=%2    WLF RMSE=%3")
                  .arg((*fit_)->c1, 0, 'f', 6)
                  .arg((*fit_)->c2, 0, 'f', 6)
                  .arg((*fit_)->rmse, 0, 'f', 6)
            : QStringLiteral("Calculate finds neighboring horizontal ln(aT) shifts by least squares.");
        painter.drawText(QRectF(18, 39, width() - 36, 22), Qt::AlignLeft | Qt::AlignVCenter, info);

        const Range rawTimeRange = computeCurveRange(XMode::Time);
        drawAxes(painter, rawTime, rawTimeRange, QStringLiteral("1a. Creep curves"), QStringLiteral("t"), QStringLiteral("epsilon"));
        drawCurves(painter, rawTime, rawTimeRange, XMode::Time);

        const Range rawLogRange = computeCurveRange(XMode::LogTime);
        drawAxes(painter, rawLog, rawLogRange, QStringLiteral("1b. Semi-log creep curves"), QStringLiteral("ln(t, s)"), QStringLiteral("epsilon"));
        drawCurves(painter, rawLog, rawLogRange, XMode::LogTime);

        const Range shiftedRange = computeCurveRange(XMode::ShiftedLogTime);
        drawAxes(painter, shifted, shiftedRange, QStringLiteral("2. Master curve coordinates"), QStringLiteral("ln(t, s) + ln(aT)"), QStringLiteral("epsilon"));
        drawCurves(painter, shifted, shiftedRange, XMode::ShiftedLogTime);

        if (fit_ && *fit_) {
            const Range shiftRange = computeShiftRange();
            drawAxes(painter, shifts, shiftRange, QStringLiteral("3. Temperature shift factors"), QStringLiteral("T - T0, C"), QStringLiteral("ln(aT)"));
            drawShiftFactors(painter, shifts, shiftRange);
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (!input_ || input_->curves.empty() || event->button() != Qt::LeftButton || !onAddPoint_) {
            return;
        }

        const QPointF pos = mousePosition(event);
        const QRectF timePlot = rawTimeRect();
        if (timePlot.contains(pos)) {
            const Range range = computeCurveRange(XMode::Time);
            const double time = range.minX + (pos.x() - timePlot.left()) * (range.maxX - range.minX) / timePlot.width();
            const double epsilon = range.minY + (timePlot.bottom() - pos.y()) * (range.maxY - range.minY) / timePlot.height();
            onAddPoint_(std::max(1e-12, time), epsilon);
            return;
        }

        const QRectF logPlot = rawLogRect();
        if (logPlot.contains(pos)) {
            const Range range = computeCurveRange(XMode::LogTime);
            const double logTime = range.minX + (pos.x() - logPlot.left()) * (range.maxX - range.minX) / logPlot.width();
            const double epsilon = range.minY + (logPlot.bottom() - pos.y()) * (range.maxY - range.minY) / logPlot.height();
            onAddPoint_(std::exp(logTime) / 3600.0, epsilon);
        }
    }

private:
    QRectF rawTimeRect() const {
        const double left = 82.0;
        const double gap = 34.0;
        const double fullWidth = std::max(240.0, width() - 108.0);
        const double halfWidth = (fullWidth - gap) / 2.0;
        const double side = std::min(halfWidth, std::max(220.0, height() * 0.28));
        return QRectF(left, 92.0, side, side);
    }

    QRectF rawLogRect() const {
        const QRectF timePlot = rawTimeRect();
        return QRectF(timePlot.right() + 34.0, timePlot.top(), timePlot.width(), timePlot.height());
    }

    QRectF shiftedRect() const {
        const QRectF raw = rawTimeRect();
        return QRectF(raw.left(), raw.bottom() + 62.0, std::max(120.0, width() - 108.0), std::max(220.0, height() * 0.26));
    }

    QRectF shiftsRect() const {
        const QRectF shifted = shiftedRect();
        return QRectF(shifted.left(), shifted.bottom() + 62.0, shifted.width(),
            std::max(130.0, height() - shifted.bottom() - 138.0));
    }

    std::vector<ShiftPoint> safeShifts() const {
        if (!fit_) {
            return {};
        }
        try {
            return deriveShiftPoints(*input_);
        } catch (...) {
            return {};
        }
    }

    double curveShift(double temperature) const {
        const auto shifts = safeShifts();
        const auto it = std::find_if(shifts.begin(), shifts.end(), [&](const ShiftPoint& p) {
            return std::abs(p.temperature - temperature) < 1e-9;
        });
        return it == shifts.end() ? 0.0 : it->logAT;
    }

    Range computeCurveRange(XMode mode) const {
        double minX = std::numeric_limits<double>::infinity();
        double maxX = -std::numeric_limits<double>::infinity();
        double minY = std::numeric_limits<double>::infinity();
        double maxY = -std::numeric_limits<double>::infinity();
        double minTime = std::numeric_limits<double>::infinity();
        double maxTime = -std::numeric_limits<double>::infinity();

        for (const auto& curve : input_->curves) {
            const double shift = mode == XMode::ShiftedLogTime ? curveShift(curve.temperature) : 0.0;
            for (const auto& point : curve.points) {
                if (point.time <= 0.0) {
                    continue;
                }
                minTime = std::min(minTime, point.time);
                maxTime = std::max(maxTime, point.time);
                const double x = mode == XMode::Time ? point.time : semiLogTime(point.time) + shift;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, point.epsilon);
                maxY = std::max(maxY, point.epsilon);
            }
        }

        const bool bookData = minTime >= 0.0 && maxTime <= 6.5 && minY >= 0.0 && maxY <= 12.5;
        if (mode == XMode::Time && bookData) {
            return makeRange(minX, maxX, minY, maxY, true);
        }
        Range range = makeRange(minX, maxX, minY, maxY);
        if (bookData) {
            range.minY = 0.0;
            range.maxY = 12.0;
            range.stepY = 2.0;
        }
        return range;
    }

    Range computeShiftRange() const {
        const auto shifts = deriveShiftPoints(*input_);
        double minT = shifts.front().temperature - input_->baseTemperature;
        double maxT = shifts.front().temperature - input_->baseTemperature;
        double minY = shifts.front().logAT;
        double maxY = shifts.front().logAT;
        for (const auto& p : shifts) {
            const double dt = p.temperature - input_->baseTemperature;
            minT = std::min(minT, dt);
            maxT = std::max(maxT, dt);
            minY = std::min(minY, p.logAT);
            maxY = std::max(maxY, p.logAT);
        }
        return makeRange(minT, maxT, minY, maxY);
    }

    QPointF toScreen(const QRectF& plot, const Range& range, double x, double y) const {
        const double px = plot.left() + (x - range.minX) * plot.width() / (range.maxX - range.minX);
        const double py = plot.bottom() - (y - range.minY) * plot.height() / (range.maxY - range.minY);
        return {px, py};
    }

    void drawAxes(QPainter& painter, const QRectF& plot, const Range& range, const QString& title, const QString& xLabel, const QString& yLabel) const {
        painter.fillRect(plot.adjusted(-1, -1, 1, 1), Qt::white);
        painter.setBrush(Qt::NoBrush);

        QPen gridPen(QColor(224, 228, 235));
        QPen framePen(QColor(55, 65, 81), 1.2);

        painter.setPen(gridPen);
        const double startX = std::ceil(range.minX / range.stepX) * range.stepX;
        for (double tx = startX; tx <= range.maxX + range.stepX * 0.25; tx += range.stepX) {
            const QPointF top = toScreen(plot, range, tx, range.maxY);
            const QPointF bottom = toScreen(plot, range, tx, range.minY);
            painter.drawLine(top, bottom);
            painter.setPen(QColor(75, 85, 99));
            painter.drawText(QRectF(bottom.x() - 46, plot.bottom() + 4, 92, 18), Qt::AlignHCenter | Qt::AlignTop,
                tickLabel(tx, range.stepX));
            painter.setPen(gridPen);
        }

        const double startY = std::ceil(range.minY / range.stepY) * range.stepY;
        for (double ty = startY; ty <= range.maxY + range.stepY * 0.25; ty += range.stepY) {
            const QPointF left = toScreen(plot, range, range.minX, ty);
            const QPointF right = toScreen(plot, range, range.maxX, ty);
            painter.drawLine(left, right);
            painter.setPen(QColor(75, 85, 99));
            painter.drawText(QRectF(plot.left() - 76, left.y() - 9, 66, 18), Qt::AlignRight | Qt::AlignVCenter,
                tickLabel(ty, range.stepY));
            painter.setPen(gridPen);
        }

        if (range.minY <= 0.0 && range.maxY >= 0.0) {
            painter.setPen(QPen(QColor(17, 24, 39), 1.2));
            painter.drawLine(toScreen(plot, range, range.minX, 0.0), toScreen(plot, range, range.maxX, 0.0));
        }

        painter.setPen(framePen);
        painter.drawRect(plot);

        QFont titleFont = painter.font();
        titleFont.setBold(true);
        painter.setFont(titleFont);
        painter.setPen(QColor(23, 32, 51));
        painter.drawText(QRectF(plot.left(), plot.top() - 30, plot.width(), 22), Qt::AlignLeft | Qt::AlignVCenter, title);

        QFont normalFont = painter.font();
        normalFont.setBold(false);
        painter.setFont(normalFont);
        painter.setPen(QColor(55, 65, 81));
        painter.drawText(QRectF(plot.center().x() - 85, plot.bottom() + 24, 170, 20), Qt::AlignCenter, xLabel);

        painter.save();
        painter.translate(plot.left() - 58, plot.center().y());
        painter.rotate(-90);
        painter.drawText(QRectF(-85, -10, 170, 20), Qt::AlignCenter, yLabel);
        painter.restore();
    }

    void drawCurves(QPainter& painter, const QRectF& plot, const Range& range, XMode mode) const {
        painter.save();
        painter.setClipRect(plot.adjusted(1, 1, -1, -1));
        if (mode == XMode::ShiftedLogTime) {
            struct MasterPoint {
                double x{};
                double y{};
                std::size_t curveIndex{};
            };
            std::vector<MasterPoint> master;
            for (std::size_t i = 0; i < input_->curves.size(); ++i) {
                const double shift = curveShift(input_->curves[i].temperature);
                for (const auto& point : input_->curves[i].points) {
                    if (point.time > 0.0) {
                        master.push_back({semiLogTime(point.time) + shift, point.epsilon, i});
                    }
                }
            }
            std::sort(master.begin(), master.end(), [](const MasterPoint& left, const MasterPoint& right) {
                return left.x < right.x;
            });

            QPainterPath path;
            bool first = true;
            for (const auto& point : master) {
                const QPointF screen = toScreen(plot, range, point.x, point.y);
                if (first) {
                    path.moveTo(screen);
                    first = false;
                } else {
                    path.lineTo(screen);
                }
            }
            painter.setPen(QPen(QColor(17, 24, 39), 2.1));
            painter.drawPath(path);

            for (const auto& point : master) {
                const QPointF screen = toScreen(plot, range, point.x, point.y);
                painter.setBrush(colorForIndex(point.curveIndex));
                painter.setPen(QPen(Qt::white, 1.0));
                painter.drawEllipse(screen, 4.2, 4.2);
            }
            painter.restore();
            return;
        }

        for (std::size_t i = 0; i < input_->curves.size(); ++i) {
            std::vector<CreepPoint> points = input_->curves[i].points;
            std::sort(points.begin(), points.end(), [](const CreepPoint& left, const CreepPoint& right) {
                return left.time < right.time;
            });
            const double shift = mode == XMode::ShiftedLogTime ? curveShift(input_->curves[i].temperature) : 0.0;
            QPainterPath path;
            bool first = true;
            for (const auto& point : points) {
                if (point.time <= 0.0) {
                    continue;
                }
                const double x = mode == XMode::Time ? point.time : semiLogTime(point.time) + shift;
                const QPointF screen = toScreen(plot, range, x, point.epsilon);
                if (first) {
                    path.moveTo(screen);
                    first = false;
                } else {
                    path.lineTo(screen);
                }
            }
            painter.setPen(QPen(colorForIndex(i), 2.2));
            painter.drawPath(path);
        }
        painter.restore();
    }

    void drawShiftFactors(QPainter& painter, const QRectF& plot, const Range& range) const {
        if (!fit_ || !*fit_) {
            return;
        }

        painter.save();
        painter.setClipRect(plot.adjusted(1, 1, -1, -1));
        {
            QPainterPath formulaPath;
            bool first = true;
            for (int i = 0; i <= 260; ++i) {
                const double dt = range.minX + (range.maxX - range.minX) * static_cast<double>(i) / 260.0;
                const double temperature = input_->baseTemperature + dt;
                const double y = modelLogAT(temperature, input_->baseTemperature, (*fit_)->c1, (*fit_)->c2);
                if (!std::isfinite(y) || std::abs((*fit_)->c2 + temperature - input_->baseTemperature) <= 1e-9) {
                    first = true;
                    continue;
                }
                const QPointF screen = toScreen(plot, range, dt, y);
                if (first) {
                    formulaPath.moveTo(screen);
                    first = false;
                } else {
                    formulaPath.lineTo(screen);
                }
            }
            painter.setPen(QPen(QColor(17, 24, 39), 2.8));
            painter.drawPath(formulaPath);
        }

        const auto points = deriveShiftPoints(*input_);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const QPointF screen = toScreen(plot, range, points[i].temperature - input_->baseTemperature, points[i].logAT);
            painter.setBrush(colorForIndex(i));
            painter.setPen(QPen(Qt::white, 1.2));
            painter.drawEllipse(screen, 5.5, 5.5);
        }
        painter.restore();
    }

    ExperimentInput* input_{};
    std::optional<FitResult>* fit_{};
    std::function<void(double, double)> onAddPoint_;
};

struct FlatRow {
    int curveIndex{};
    int pointIndex{};
};

class MainWindow final : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(QStringLiteral("TVA Creep Curve Processor"));
        resize(1240, 820);

        plot_.setState(&input_, &fit_);
        plot_.setAddCallback([this](double time, double epsilon) {
            addPointFromGraph(time, epsilon);
        });

        auto* root = new QWidget(this);
        auto* rootLayout = new QHBoxLayout(root);
        rootLayout->setContentsMargins(10, 10, 10, 10);

        auto* splitter = new QSplitter(Qt::Horizontal, root);
        splitter->addWidget(createEditorPanel());
        splitter->addWidget(&plot_);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({390, 850});
        rootLayout->addWidget(splitter);
        setCentralWidget(root);

        refreshForm();
    }

private:
    QWidget* createEditorPanel() {
        auto* panel = new QWidget(this);
        panel->setMinimumWidth(380);
        auto* layout = new QVBoxLayout(panel);

        auto* fileRow = new QGridLayout();
        auto* loadButton = new QPushButton(QStringLiteral("Load XML"), panel);
        auto* saveXmlButton = new QPushButton(QStringLiteral("Save XML"), panel);
        auto* calculateButton = new QPushButton(QStringLiteral("Calculate"), panel);
        auto* saveResultButton = new QPushButton(QStringLiteral("Save Result"), panel);
        auto* saveSvgButton = new QPushButton(QStringLiteral("Save SVG"), panel);
        fileRow->addWidget(loadButton, 0, 0);
        fileRow->addWidget(saveXmlButton, 0, 1);
        fileRow->addWidget(calculateButton, 0, 2);
        fileRow->addWidget(saveResultButton, 1, 0, 1, 2);
        fileRow->addWidget(saveSvgButton, 1, 2);
        layout->addLayout(fileRow);

        auto* inputBox = new QGroupBox(QStringLiteral("Experiment"), panel);
        auto* form = new QFormLayout(inputBox);
        experimentEdit_ = new QLineEdit(inputBox);
        baseEdit_ = new QLineEdit(inputBox);
        baseEdit_->setValidator(new QDoubleValidator(baseEdit_));
        form->addRow(QStringLiteral("ID"), experimentEdit_);
        form->addRow(QStringLiteral("T0"), baseEdit_);
        layout->addWidget(inputBox);

        table_ = new QTableWidget(panel);
        table_->setColumnCount(3);
        table_->setHorizontalHeaderLabels({QStringLiteral("Temperature"), QStringLiteral("Time"), QStringLiteral("Epsilon")});
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table_->verticalHeader()->setVisible(false);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        layout->addWidget(table_, 1);

        auto* pointBox = new QGroupBox(QStringLiteral("Creep point"), panel);
        auto* pointLayout = new QGridLayout(pointBox);
        pointTempEdit_ = new QLineEdit(pointBox);
        pointTimeEdit_ = new QLineEdit(pointBox);
        pointEpsEdit_ = new QLineEdit(pointBox);
        pointTempEdit_->setValidator(new QDoubleValidator(pointTempEdit_));
        pointTimeEdit_->setValidator(new QDoubleValidator(pointTimeEdit_));
        pointEpsEdit_->setValidator(new QDoubleValidator(pointEpsEdit_));
        auto* addButton = new QPushButton(QStringLiteral("Add"), pointBox);
        auto* updateButton = new QPushButton(QStringLiteral("Update"), pointBox);
        auto* deleteButton = new QPushButton(QStringLiteral("Delete"), pointBox);
        pointLayout->addWidget(new QLabel(QStringLiteral("Temperature"), pointBox), 0, 0);
        pointLayout->addWidget(pointTempEdit_, 0, 1, 1, 2);
        pointLayout->addWidget(new QLabel(QStringLiteral("Time"), pointBox), 1, 0);
        pointLayout->addWidget(pointTimeEdit_, 1, 1, 1, 2);
        pointLayout->addWidget(new QLabel(QStringLiteral("Epsilon"), pointBox), 2, 0);
        pointLayout->addWidget(pointEpsEdit_, 2, 1, 1, 2);
        pointLayout->addWidget(addButton, 3, 0);
        pointLayout->addWidget(updateButton, 3, 1);
        pointLayout->addWidget(deleteButton, 3, 2);
        layout->addWidget(pointBox);

        shiftTable_ = new QTableWidget(panel);
        shiftTable_->setColumnCount(4);
        shiftTable_->setHorizontalHeaderLabels({QStringLiteral("T"), QStringLiteral("ln(aT)"), QStringLiteral("curve RMSE"), QStringLiteral("overlap")});
        shiftTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        shiftTable_->verticalHeader()->setVisible(false);
        shiftTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        shiftTable_->setMaximumHeight(160);
        layout->addWidget(shiftTable_);

        resultLabel_ = new QLabel(panel);
        resultLabel_->setFrameShape(QFrame::StyledPanel);
        resultLabel_->setMinimumHeight(78);
        resultLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        layout->addWidget(resultLabel_);

        connect(loadButton, &QPushButton::clicked, this, [this]() { loadXml(); });
        connect(saveXmlButton, &QPushButton::clicked, this, [this]() { saveXml(); });
        connect(calculateButton, &QPushButton::clicked, this, [this]() { calculate(); });
        connect(saveResultButton, &QPushButton::clicked, this, [this]() { saveResult(); });
        connect(saveSvgButton, &QPushButton::clicked, this, [this]() { saveSvg(); });
        connect(addButton, &QPushButton::clicked, this, [this]() { addPointFromEdits(); });
        connect(updateButton, &QPushButton::clicked, this, [this]() { updateSelectedPointFromEdits(); });
        connect(deleteButton, &QPushButton::clicked, this, [this]() { deleteSelectedPoint(); });
        connect(table_, &QTableWidget::cellChanged, this, [this](int row, int column) { tableCellChanged(row, column); });
        connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() { tableSelectionChanged(); });
        connect(experimentEdit_, &QLineEdit::editingFinished, this, [this]() { syncHeaderFromEdits(); });
        connect(baseEdit_, &QLineEdit::editingFinished, this, [this]() { syncHeaderFromEdits(); });

        return panel;
    }

    void showError(const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("TVA"), qstr(error.what()));
    }

    void syncHeaderFromEdits() {
        input_.experimentId = utf8(experimentEdit_->text());
        input_.baseTemperature = baseEdit_->text().toDouble();
        fit_.reset();
        refreshResult();
        refreshShiftTable();
        plot_.update();
    }

    void rebuildFlatRows() {
        flatRows_.clear();
        for (std::size_t ci = 0; ci < input_.curves.size(); ++ci) {
            for (std::size_t pi = 0; pi < input_.curves[ci].points.size(); ++pi) {
                flatRows_.push_back({static_cast<int>(ci), static_cast<int>(pi)});
            }
        }
    }

    void refreshForm() {
        rebuildFlatRows();
        updatingTable_ = true;
        experimentEdit_->setText(qstr(input_.experimentId));
        baseEdit_->setText(qstr(formatCompact(input_.baseTemperature)));
        table_->setRowCount(static_cast<int>(flatRows_.size()));
        for (int row = 0; row < table_->rowCount(); ++row) {
            const auto fr = flatRows_[static_cast<std::size_t>(row)];
            const auto& curve = input_.curves[static_cast<std::size_t>(fr.curveIndex)];
            const auto& point = curve.points[static_cast<std::size_t>(fr.pointIndex)];
            table_->setItem(row, 0, new QTableWidgetItem(qstr(formatCompact(curve.temperature))));
            table_->setItem(row, 1, new QTableWidgetItem(qstr(formatCompact(point.time))));
            table_->setItem(row, 2, new QTableWidgetItem(qstr(formatCompact(point.epsilon))));
        }
        updatingTable_ = false;
        if (selectedRow_ >= 0 && selectedRow_ < table_->rowCount()) {
            table_->selectRow(selectedRow_);
        }
        refreshPointEdits();
        refreshShiftTable();
        refreshResult();
        plot_.update();
    }

    void refreshShiftTable() {
        shiftTable_->setRowCount(0);
        if (!fit_) {
            return;
        }
        try {
            const auto shifts = deriveShiftPoints(input_);
            shiftTable_->setRowCount(static_cast<int>(shifts.size()));
            for (int row = 0; row < shiftTable_->rowCount(); ++row) {
                const auto& point = shifts[static_cast<std::size_t>(row)];
                shiftTable_->setItem(row, 0, new QTableWidgetItem(qstr(formatCompact(point.temperature))));
                shiftTable_->setItem(row, 1, new QTableWidgetItem(qstr(formatCompact(point.logAT))));
                shiftTable_->setItem(row, 2, new QTableWidgetItem(qstr(formatCompact(point.rmse))));
                shiftTable_->setItem(row, 3, new QTableWidgetItem(QString::number(point.overlapCount)));
            }
        } catch (...) {
        }
    }

    void refreshResult() {
        if (!fit_) {
            resultLabel_->setText(QStringLiteral("No calculation yet.\nln(aT) is fitted by neighboring horizontal curve shifts."));
            return;
        }
        const double refTemp = input_.curves[static_cast<std::size_t>(fit_->referenceCurveIndex)].temperature;
        resultLabel_->setText(QStringLiteral("Reference curve T = %1\nC1 = %2\nC2 = %3\nWLF RMSE = %4")
                                  .arg(refTemp, 0, 'f', 3)
                                  .arg(fit_->c1, 0, 'f', 6)
                                  .arg(fit_->c2, 0, 'f', 6)
                                  .arg(fit_->rmse, 0, 'f', 6));
    }

    void refreshPointEdits() {
        if (selectedRow_ < 0 || selectedRow_ >= static_cast<int>(flatRows_.size())) {
            return;
        }
        const auto fr = flatRows_[static_cast<std::size_t>(selectedRow_)];
        const auto& curve = input_.curves[static_cast<std::size_t>(fr.curveIndex)];
        const auto& point = curve.points[static_cast<std::size_t>(fr.pointIndex)];
        pointTempEdit_->setText(qstr(formatCompact(curve.temperature)));
        pointTimeEdit_->setText(qstr(formatCompact(point.time)));
        pointEpsEdit_->setText(qstr(formatCompact(point.epsilon)));
    }

    int findOrCreateCurve(double temperature) {
        for (std::size_t i = 0; i < input_.curves.size(); ++i) {
            if (std::abs(input_.curves[i].temperature - temperature) < 1e-9) {
                return static_cast<int>(i);
            }
        }
        input_.curves.push_back({temperature, {}});
        std::sort(input_.curves.begin(), input_.curves.end(), [](const CreepCurve& left, const CreepCurve& right) {
            return left.temperature < right.temperature;
        });
        for (std::size_t i = 0; i < input_.curves.size(); ++i) {
            if (std::abs(input_.curves[i].temperature - temperature) < 1e-9) {
                return static_cast<int>(i);
            }
        }
        return static_cast<int>(input_.curves.size()) - 1;
    }

    double currentPointTemperature() const {
        bool ok = false;
        const double edited = pointTempEdit_->text().toDouble(&ok);
        if (ok) {
            return edited;
        }
        if (selectedRow_ >= 0 && selectedRow_ < static_cast<int>(flatRows_.size())) {
            const auto fr = flatRows_[static_cast<std::size_t>(selectedRow_)];
            return input_.curves[static_cast<std::size_t>(fr.curveIndex)].temperature;
        }
        return input_.baseTemperature;
    }

    void addPointFromGraph(double time, double epsilon) {
        try {
            syncHeaderFromEdits();
            const double temperature = currentPointTemperature();
            const int curveIndex = findOrCreateCurve(temperature);
            input_.curves[static_cast<std::size_t>(curveIndex)].points.push_back({std::max(1e-12, time), epsilon});
            pointTempEdit_->setText(qstr(formatCompact(temperature)));
            pointTimeEdit_->setText(qstr(formatCompact(time)));
            pointEpsEdit_->setText(qstr(formatCompact(epsilon)));
            fit_.reset();
            selectedRow_ = -1;
            refreshForm();
        } catch (const std::exception& e) {
            showError(e);
        }
    }

    void addPointFromEdits() {
        try {
            syncHeaderFromEdits();
            const double temperature = pointTempEdit_->text().toDouble();
            const double time = std::max(1e-12, pointTimeEdit_->text().toDouble());
            const double epsilon = pointEpsEdit_->text().toDouble();
            const int curveIndex = findOrCreateCurve(temperature);
            input_.curves[static_cast<std::size_t>(curveIndex)].points.push_back({time, epsilon});
            fit_.reset();
            selectedRow_ = -1;
            refreshForm();
        } catch (const std::exception& e) {
            showError(e);
        }
    }

    void updateSelectedPointFromEdits() {
        try {
            if (selectedRow_ < 0 || selectedRow_ >= static_cast<int>(flatRows_.size())) {
                throw std::runtime_error("Select a point first.");
            }
            syncHeaderFromEdits();
            const auto fr = flatRows_[static_cast<std::size_t>(selectedRow_)];
            const double newTemperature = pointTempEdit_->text().toDouble();
            const CreepPoint point{std::max(1e-12, pointTimeEdit_->text().toDouble()), pointEpsEdit_->text().toDouble()};
            auto& oldCurve = input_.curves[static_cast<std::size_t>(fr.curveIndex)];
            oldCurve.points.erase(oldCurve.points.begin() + fr.pointIndex);
            const int targetCurve = findOrCreateCurve(newTemperature);
            input_.curves[static_cast<std::size_t>(targetCurve)].points.push_back(point);
            removeEmptyCurves();
            fit_.reset();
            selectedRow_ = -1;
            refreshForm();
        } catch (const std::exception& e) {
            showError(e);
        }
    }

    void deleteSelectedPoint() {
        try {
            if (selectedRow_ < 0 || selectedRow_ >= static_cast<int>(flatRows_.size())) {
                throw std::runtime_error("Select a point first.");
            }
            const auto fr = flatRows_[static_cast<std::size_t>(selectedRow_)];
            auto& curve = input_.curves[static_cast<std::size_t>(fr.curveIndex)];
            curve.points.erase(curve.points.begin() + fr.pointIndex);
            removeEmptyCurves();
            fit_.reset();
            selectedRow_ = -1;
            refreshForm();
        } catch (const std::exception& e) {
            showError(e);
        }
    }

    void removeEmptyCurves() {
        input_.curves.erase(std::remove_if(input_.curves.begin(), input_.curves.end(), [](const CreepCurve& curve) {
            return curve.points.empty();
        }), input_.curves.end());
    }

    void tableCellChanged(int row, int column) {
        if (updatingTable_ || row < 0 || row >= static_cast<int>(flatRows_.size())) {
            return;
        }
        bool ok = false;
        const double value = table_->item(row, column)->text().toDouble(&ok);
        if (!ok) {
            return;
        }

        const auto fr = flatRows_[static_cast<std::size_t>(row)];
        auto& curve = input_.curves[static_cast<std::size_t>(fr.curveIndex)];
        auto& point = curve.points[static_cast<std::size_t>(fr.pointIndex)];
        if (column == 0) {
            curve.temperature = value;
        } else if (column == 1) {
            point.time = std::max(1e-12, value);
        } else {
            point.epsilon = value;
        }
        selectedRow_ = row;
        fit_.reset();
        refreshPointEdits();
        refreshShiftTable();
        refreshResult();
        plot_.update();
    }

    void tableSelectionChanged() {
        const int row = table_->currentRow();
        if (row >= 0 && row < static_cast<int>(flatRows_.size())) {
            selectedRow_ = row;
            refreshPointEdits();
        }
    }

    void loadXml() {
        try {
            const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load XML"), QString(), QStringLiteral("XML files (*.xml);;All files (*.*)"));
            if (path.isEmpty()) {
                return;
            }
            input_ = parseExperimentXml(utf8(path));
            currentXmlPath_ = path;
            fit_.reset();
            selectedRow_ = input_.curves.empty() ? -1 : 0;
            refreshForm();
        } catch (const std::exception& e) {
            showError(e);
        }
    }

    void saveXml() {
        try {
            syncHeaderFromEdits();
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save XML"), currentXmlPath_, QStringLiteral("XML files (*.xml);;All files (*.*)"));
            if (path.isEmpty()) {
                return;
            }
            writeExperimentXml(utf8(path), input_);
            currentXmlPath_ = path;
            QMessageBox::information(this, QStringLiteral("TVA"), QStringLiteral("XML saved."));
        } catch (const std::exception& e) {
            showError(e);
        }
    }

    void calculate() {
        try {
            syncHeaderFromEdits();
            fit_ = fitWlf(input_);
            refreshShiftTable();
            refreshResult();
            plot_.update();
        } catch (const std::exception& e) {
            showError(e);
        }
    }

    void saveResult() {
        try {
            if (!fit_) {
                syncHeaderFromEdits();
                fit_ = fitWlf(input_);
                refreshShiftTable();
                refreshResult();
            }
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save Result"), QStringLiteral("results.db"),
                QStringLiteral("SQLite DB (*.db);;All files (*.*)"));
            if (path.isEmpty()) {
                return;
            }
            writeResultDatabase(utf8(path), input_, *fit_);
            QMessageBox::information(this, QStringLiteral("TVA"), qstr(resultStorageDescription(utf8(path))));
        } catch (const std::exception& e) {
            showError(e);
        }
    }

    void saveSvg() {
        try {
            if (!fit_) {
                syncHeaderFromEdits();
                fit_ = fitWlf(input_);
                refreshShiftTable();
                refreshResult();
            }
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save SVG"), QStringLiteral("plot.svg"),
                QStringLiteral("SVG graph (*.svg);;All files (*.*)"));
            if (path.isEmpty()) {
                return;
            }
            writePlotSvg(utf8(path), input_, *fit_);
            QMessageBox::information(this, QStringLiteral("TVA"), QStringLiteral("SVG plot saved."));
        } catch (const std::exception& e) {
            showError(e);
        }
    }

    ExperimentInput input_{"book_creep_curves", 20.0,
        {
            {-20.0, {{0.05, 0.35}, {0.10, 0.55}, {0.20, 0.70}, {0.30, 0.82}, {0.50, 0.95}, {0.75, 1.05}, {1.00, 1.10}, {1.50, 1.13}, {2.00, 1.15}, {3.00, 1.17}, {4.00, 1.18}, {5.00, 1.19}, {6.00, 1.20}}},
            {-10.0, {{0.05, 0.60}, {0.10, 0.90}, {0.20, 1.10}, {0.30, 1.25}, {0.50, 1.38}, {0.75, 1.48}, {1.00, 1.55}, {1.50, 1.60}, {2.00, 1.65}, {3.00, 1.68}, {4.00, 1.72}, {5.00, 1.75}, {6.00, 1.78}}},
            {0.0, {{0.05, 0.85}, {0.10, 1.15}, {0.20, 1.40}, {0.30, 1.55}, {0.50, 1.72}, {0.75, 1.85}, {1.00, 1.95}, {1.50, 2.00}, {2.00, 2.05}, {3.00, 2.08}, {4.00, 2.10}, {5.00, 2.12}, {6.00, 2.15}}},
            {10.0, {{0.05, 1.50}, {0.10, 1.80}, {0.20, 2.05}, {0.30, 2.20}, {0.50, 2.45}, {0.75, 2.60}, {1.00, 2.72}, {1.50, 2.86}, {2.00, 2.92}, {3.00, 2.97}, {4.00, 3.00}, {5.00, 3.04}, {6.00, 3.08}}},
            {20.0, {{0.05, 2.05}, {0.10, 2.35}, {0.20, 2.75}, {0.30, 3.00}, {0.50, 3.25}, {0.75, 3.45}, {1.00, 3.58}, {1.50, 3.68}, {2.00, 3.75}, {3.00, 3.78}, {4.00, 3.82}, {5.00, 3.86}, {6.00, 3.90}}},
            {30.0, {{0.05, 2.50}, {0.10, 3.00}, {0.20, 3.35}, {0.30, 3.60}, {0.50, 3.95}, {0.75, 4.15}, {1.00, 4.30}, {1.50, 4.62}, {2.00, 4.90}, {3.00, 5.10}, {4.00, 5.25}, {5.00, 5.38}, {6.00, 5.50}}},
            {40.0, {{0.05, 5.20}, {0.10, 5.55}, {0.20, 5.95}, {0.30, 6.30}, {0.50, 6.55}, {0.75, 6.75}, {1.00, 6.90}, {1.50, 7.12}, {2.00, 7.25}, {3.00, 7.45}, {4.00, 7.55}, {5.00, 7.63}, {6.00, 7.75}}},
            {45.0, {{0.05, 6.50}, {0.10, 7.40}, {0.20, 8.00}, {0.30, 8.45}, {0.50, 8.90}, {0.75, 9.40}, {1.00, 9.80}, {1.50, 10.55}, {2.00, 10.90}, {3.00, 11.35}, {4.00, 11.55}, {5.00, 11.72}, {6.00, 11.90}}}
        }};
    std::optional<FitResult> fit_;
    QString currentXmlPath_;
    std::vector<FlatRow> flatRows_;
    int selectedRow_{0};
    bool updatingTable_{false};

    PlotWidget plot_;
    QLineEdit* experimentEdit_{};
    QLineEdit* baseEdit_{};
    QTableWidget* table_{};
    QTableWidget* shiftTable_{};
    QLineEdit* pointTempEdit_{};
    QLineEdit* pointTimeEdit_{};
    QLineEdit* pointEpsEdit_{};
    QLabel* resultLabel_{};
};

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
