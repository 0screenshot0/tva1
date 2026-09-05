#include "tva_core.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>

#ifdef TVA_USE_SQLITE
#include <sqlite3.h>
#endif

double semiLogTime(double timeHours) {
    if (timeHours <= 0.0 || !std::isfinite(timeHours)) {
        throw std::runtime_error("All creep points must have positive finite time");
    }
    return std::log(timeHours * 3600.0);
}

namespace {

struct PlotArea {
    double x{};
    double y{};
    double width{};
    double height{};
};

struct SemiLogPoint {
    double x{};
    double y{};
};

std::string xmlEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

bool tryExtractAttribute(const std::string& source, const std::string& key, std::string& value) {
    const std::regex attrRegex(key + "\\s*=\\s*\"([^\"]+)\"");
    std::smatch m;
    if (std::regex_search(source, m, attrRegex) && m.size() > 1) {
        value = m[1].str();
        return true;
    }
    return false;
}

std::string extractAttribute(const std::string& source, const std::string& key) {
    std::string value;
    if (tryExtractAttribute(source, key, value)) {
        return value;
    }
    throw std::runtime_error("Missing attribute: " + key);
}

std::string extractAnyAttribute(const std::string& source, const std::vector<std::string>& keys) {
    std::string value;
    for (const auto& key : keys) {
        if (tryExtractAttribute(source, key, value)) {
            return value;
        }
    }

    std::ostringstream msg;
    msg << "Missing attribute. Expected one of: ";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i != 0) {
            msg << ", ";
        }
        msg << keys[i];
    }
    throw std::runtime_error(msg.str());
}

std::string findBlock(const std::string& source, std::size_t openEnd, const std::string& tag) {
    const std::string closeTag = "</" + tag + ">";
    const std::size_t closePos = source.find(closeTag, openEnd);
    if (closePos == std::string::npos) {
        throw std::runtime_error("Cannot find closing tag: " + closeTag);
    }
    return source.substr(openEnd, closePos - openEnd);
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

std::vector<SemiLogPoint> toSemiLogPoints(const CreepCurve& curve) {
    std::vector<SemiLogPoint> out;
    out.reserve(curve.points.size());
    for (const auto& p : curve.points) {
        if (!std::isfinite(p.epsilon)) {
            throw std::runtime_error("All creep points must have finite epsilon");
        }
        out.push_back({semiLogTime(p.time), p.epsilon});
    }
    std::sort(out.begin(), out.end(), [](const SemiLogPoint& left, const SemiLogPoint& right) {
        return left.x < right.x;
    });
    return out;
}

bool interpolateY(const std::vector<SemiLogPoint>& curve, double x, double& y) {
    if (curve.size() < 2 || x < curve.front().x || x > curve.back().x) {
        return false;
    }
    auto upper = std::lower_bound(curve.begin(), curve.end(), x, [](const SemiLogPoint& p, double value) {
        return p.x < value;
    });
    if (upper == curve.begin()) {
        y = upper->y;
        return true;
    }
    if (upper == curve.end()) {
        y = curve.back().y;
        return true;
    }
    const auto& right = *upper;
    const auto& left = *(upper - 1);
    const double dx = right.x - left.x;
    if (std::abs(dx) < 1e-12) {
        y = left.y;
        return true;
    }
    const double k = (x - left.x) / dx;
    y = left.y + k * (right.y - left.y);
    return true;
}

int referenceCurveIndex(const ExperimentInput& input) {
    if (input.curves.empty()) {
        throw std::runtime_error("No creep curves in experiment");
    }
    int best = 0;
    double bestDistance = std::abs(input.curves[0].temperature - input.baseTemperature);
    for (std::size_t i = 1; i < input.curves.size(); ++i) {
        const double distance = std::abs(input.curves[i].temperature - input.baseTemperature);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

double shiftMse(
    const std::vector<SemiLogPoint>& curve,
    const std::vector<SemiLogPoint>& reference,
    double shift,
    int& overlapCount) {
    double sum = 0.0;
    overlapCount = 0;
    for (const auto& p : curve) {
        double refY = 0.0;
        if (interpolateY(reference, p.x + shift, refY)) {
            const double error = p.y - refY;
            sum += error * error;
            ++overlapCount;
        }
    }
    if (overlapCount < 2) {
        return std::numeric_limits<double>::infinity();
    }
    return sum / static_cast<double>(overlapCount);
}

ShiftPoint findBestShift(const CreepCurve& curve, const std::vector<SemiLogPoint>& reference) {
    const std::vector<SemiLogPoint> semi = toSemiLogPoints(curve);
    if (semi.size() < 2) {
        throw std::runtime_error("Each creep curve must contain at least 2 points");
    }

    const double minShift = reference.front().x - semi.back().x;
    const double maxShift = reference.back().x - semi.front().x;
    if (minShift > maxShift) {
        throw std::runtime_error("Creep curve has no possible overlap with reference curve");
    }

    constexpr int gridSteps = 240;
    double bestShift = minShift;
    double bestMse = std::numeric_limits<double>::infinity();
    int bestCount = 0;
    int bestGridIndex = 0;
    for (int i = 0; i <= gridSteps; ++i) {
        const double shift = minShift + (maxShift - minShift) * static_cast<double>(i) / gridSteps;
        int count = 0;
        const double mse = shiftMse(semi, reference, shift, count);
        if (mse < bestMse) {
            bestMse = mse;
            bestShift = shift;
            bestCount = count;
            bestGridIndex = i;
        }
    }

    if (!std::isfinite(bestMse)) {
        throw std::runtime_error("Cannot find horizontal shift: not enough overlapping points");
    }

    double left = minShift + (maxShift - minShift) * static_cast<double>(std::max(0, bestGridIndex - 1)) / gridSteps;
    double right = minShift + (maxShift - minShift) * static_cast<double>(std::min(gridSteps, bestGridIndex + 1)) / gridSteps;
    if (std::abs(right - left) < 1e-12) {
        left = minShift;
        right = maxShift;
    }

    constexpr double golden = 0.6180339887498948;
    for (int iter = 0; iter < 80; ++iter) {
        const double x1 = right - golden * (right - left);
        const double x2 = left + golden * (right - left);
        int c1 = 0;
        int c2 = 0;
        const double f1 = shiftMse(semi, reference, x1, c1);
        const double f2 = shiftMse(semi, reference, x2, c2);
        if (f1 < f2) {
            right = x2;
        } else {
            left = x1;
        }
    }

    const double refinedShift = (left + right) / 2.0;
    int refinedCount = 0;
    const double refinedMse = shiftMse(semi, reference, refinedShift, refinedCount);
    if (std::isfinite(refinedMse) && refinedMse <= bestMse) {
        bestShift = refinedShift;
        bestMse = refinedMse;
        bestCount = refinedCount;
    }

    return {curve.temperature, bestShift, std::sqrt(bestMse), bestCount};
}

std::string svgNumber(double value, int precision = 3) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

double mapX(double value, const PlotArea& area, double minX, double maxX) {
    return area.x + (value - minX) * area.width / (maxX - minX);
}

double mapY(double value, const PlotArea& area, double minY, double maxY) {
    return area.y + area.height - (value - minY) * area.height / (maxY - minY);
}

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

std::string tickLabel(double value, double step) {
    const double normalized = std::abs(value) < std::abs(step) * 1e-6 ? 0.0 : value;
    int precision = 0;
    if (std::abs(step) < 1.0) {
        precision = std::abs(step) < 0.01 ? 3 : (std::abs(step) < 0.1 ? 2 : 1);
    }
    return svgNumber(normalized, precision);
}

void writeSvgAxes(
    std::ostream& out,
    const PlotArea& area,
    double minX,
    double maxX,
    double minY,
    double maxY,
    const std::string& title,
    const std::string& xLabel,
    const std::string& yLabel) {
    out << "<text x=\"" << svgNumber(area.x) << "\" y=\"" << svgNumber(area.y - 24)
        << "\" class=\"title\">" << xmlEscape(title) << "</text>\n";
    out << "<text x=\"" << svgNumber(area.x + area.width / 2) << "\" y=\""
        << svgNumber(area.y + area.height + 40)
        << "\" class=\"axis-label\" text-anchor=\"middle\">" << xmlEscape(xLabel) << "</text>\n";
    out << "<text x=\"" << svgNumber(area.x - 54) << "\" y=\"" << svgNumber(area.y + area.height / 2)
        << "\" class=\"axis-label\" transform=\"rotate(-90 " << svgNumber(area.x - 54) << " "
        << svgNumber(area.y + area.height / 2) << ")\">" << xmlEscape(yLabel) << "</text>\n";

    out << "<rect x=\"" << svgNumber(area.x) << "\" y=\"" << svgNumber(area.y)
        << "\" width=\"" << svgNumber(area.width) << "\" height=\"" << svgNumber(area.height)
        << "\" class=\"plot-bg\"/>\n";

    const double stepX = niceStep(maxX - minX);
    const double stepY = niceStep(maxY - minY);
    const double startX = std::ceil(minX / stepX) * stepX;
    for (double tx = startX; tx <= maxX + stepX * 0.25; tx += stepX) {
        const double px = mapX(tx, area, minX, maxX);
        out << "<line x1=\"" << svgNumber(px) << "\" y1=\"" << svgNumber(area.y)
            << "\" x2=\"" << svgNumber(px) << "\" y2=\"" << svgNumber(area.y + area.height)
            << "\" class=\"grid\"/>\n";
        out << "<text x=\"" << svgNumber(px) << "\" y=\"" << svgNumber(area.y + area.height + 23)
            << "\" class=\"tick\" text-anchor=\"middle\">" << tickLabel(tx, stepX) << "</text>\n";
    }

    const double startY = std::ceil(minY / stepY) * stepY;
    for (double ty = startY; ty <= maxY + stepY * 0.25; ty += stepY) {
        const double py = mapY(ty, area, minY, maxY);
        out << "<line x1=\"" << svgNumber(area.x) << "\" y1=\"" << svgNumber(py)
            << "\" x2=\"" << svgNumber(area.x + area.width) << "\" y2=\"" << svgNumber(py)
            << "\" class=\"grid\"/>\n";
        out << "<text x=\"" << svgNumber(area.x - 12) << "\" y=\"" << svgNumber(py + 4)
            << "\" class=\"tick\" text-anchor=\"end\">" << tickLabel(ty, stepY) << "</text>\n";
    }

    if (minY <= 0.0 && maxY >= 0.0) {
        const double y0 = mapY(0.0, area, minY, maxY);
        out << "<line x1=\"" << svgNumber(area.x) << "\" y1=\"" << svgNumber(y0)
            << "\" x2=\"" << svgNumber(area.x + area.width) << "\" y2=\"" << svgNumber(y0)
            << "\" class=\"zero-line\"/>\n";
    }

    out << "<rect x=\"" << svgNumber(area.x) << "\" y=\"" << svgNumber(area.y)
        << "\" width=\"" << svgNumber(area.width) << "\" height=\"" << svgNumber(area.height)
        << "\" class=\"frame\"/>\n";
}

std::string colorForIndex(std::size_t index) {
    static const char* colors[] = {"#225ea8", "#e2533f", "#268b5c", "#8b5cf6", "#c47f00", "#0f766e", "#be185d", "#111827"};
    return colors[index % (sizeof(colors) / sizeof(colors[0]))];
}

} // namespace

std::string readTextFile(const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::in | std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

ExperimentInput parseExperimentXml(const std::string& xmlPath) {
    const std::string xml = readTextFile(xmlPath);

    std::smatch rootMatch;
    const std::regex rootRegex(R"(<\s*Experiment\b([^>]*)>)");
    if (!std::regex_search(xml, rootMatch, rootRegex) || rootMatch.size() < 2) {
        throw std::runtime_error("Cannot find <Experiment ...> root node");
    }

    ExperimentInput input;
    const std::string rootAttrs = rootMatch[1].str();
    input.experimentId = extractAttribute(rootAttrs, "id");
    input.baseTemperature = std::stod(
        extractAnyAttribute(rootAttrs, {"baseTemperature", "referenceTemperature", "t0"}));

    const std::regex curveRegex(R"(<\s*Curve\b([^>]*)>)");
    for (std::sregex_iterator it(xml.begin(), xml.end(), curveRegex), end; it != end; ++it) {
        const std::string attrs = (*it)[1].str();
        const std::size_t openEnd = static_cast<std::size_t>(it->position() + it->length());
        const std::string block = findBlock(xml, openEnd, "Curve");

        CreepCurve curve;
        curve.temperature = std::stod(extractAnyAttribute(attrs, {"temperature", "T"}));

        const std::regex pointRegex(R"(<\s*Point\b([^>]*)/>)");
        for (std::sregex_iterator pit(block.begin(), block.end(), pointRegex), pend; pit != pend; ++pit) {
            const std::string pointAttrs = (*pit)[1].str();
            CreepPoint point;
            point.time = std::stod(extractAnyAttribute(pointAttrs, {"time", "t"}));
            point.epsilon = std::stod(extractAnyAttribute(pointAttrs, {"epsilon", "strain", "eps"}));
            curve.points.push_back(point);
        }

        if (curve.points.size() < 2) {
            throw std::runtime_error("Each <Curve> must contain at least 2 points");
        }
        input.curves.push_back(curve);
    }

    if (input.curves.size() < 3) {
        throw std::runtime_error("Need at least 3 temperature creep curves for WLF fit");
    }
    for (const auto& curve : input.curves) {
        (void)toSemiLogPoints(curve);
    }
    return input;
}

void writeExperimentXml(const std::string& filePath, const ExperimentInput& input) {
    std::ofstream out(std::filesystem::u8path(filePath));
    if (!out) {
        throw std::runtime_error("Cannot write XML file: " + filePath);
    }

    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<Experiment id=\"" << xmlEscape(input.experimentId) << "\" baseTemperature=\""
        << std::setprecision(12) << input.baseTemperature << "\">\n";
    out << "  <Curves>\n";
    for (const auto& curve : input.curves) {
        out << "    <Curve temperature=\"" << std::setprecision(12) << curve.temperature << "\">\n";
        for (const auto& point : curve.points) {
            out << "      <Point time=\"" << std::setprecision(12) << point.time
                << "\" epsilon=\"" << std::setprecision(12) << point.epsilon << "\"/>\n";
        }
        out << "    </Curve>\n";
    }
    out << "  </Curves>\n";
    out << "</Experiment>\n";
}

std::vector<ShiftPoint> deriveShiftPoints(const ExperimentInput& input) {
    if (input.curves.empty()) {
        throw std::runtime_error("No creep curves in experiment");
    }

    std::vector<std::size_t> order(input.curves.size());
    for (std::size_t i = 0; i < input.curves.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return input.curves[left].temperature < input.curves[right].temperature;
    });

    int refOrderIndex = 0;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < order.size(); ++i) {
        const double distance = std::abs(input.curves[order[i]].temperature - input.baseTemperature);
        if (distance < bestDistance) {
            bestDistance = distance;
            refOrderIndex = static_cast<int>(i);
        }
    }

    std::vector<ShiftPoint> shifts(order.size());
    const auto& referenceCurve = input.curves[order[static_cast<std::size_t>(refOrderIndex)]];
    shifts[static_cast<std::size_t>(refOrderIndex)] = {
        referenceCurve.temperature,
        0.0,
        0.0,
        static_cast<int>(referenceCurve.points.size())
    };

    for (int i = refOrderIndex + 1; i < static_cast<int>(order.size()); ++i) {
        const auto& current = input.curves[order[static_cast<std::size_t>(i)]];
        const auto& previous = input.curves[order[static_cast<std::size_t>(i - 1)]];
        ShiftPoint pairShift = findBestShift(current, toSemiLogPoints(previous));
        pairShift.logAT += shifts[static_cast<std::size_t>(i - 1)].logAT;
        shifts[static_cast<std::size_t>(i)] = pairShift;
    }

    for (int i = refOrderIndex - 1; i >= 0; --i) {
        const auto& current = input.curves[order[static_cast<std::size_t>(i)]];
        const auto& next = input.curves[order[static_cast<std::size_t>(i + 1)]];
        ShiftPoint pairShift = findBestShift(current, toSemiLogPoints(next));
        pairShift.logAT += shifts[static_cast<std::size_t>(i + 1)].logAT;
        shifts[static_cast<std::size_t>(i)] = pairShift;
    }

    return shifts;
}

double modelLogAT(double temperature, double t0, double c1, double c2) {
    const double dt = temperature - t0;
    const double denom = c2 + dt;
    if (std::abs(denom) < 1e-12) {
        return c1 * dt / ((denom >= 0.0) ? 1e-12 : -1e-12);
    }
    return c1 * dt / denom;
}

static double fitMse(const std::vector<ShiftPoint>& shifts, double t0, double c1, double c2) {
    double sum = 0.0;
    for (const auto& p : shifts) {
        const double error = modelLogAT(p.temperature, t0, c1, c2) - p.logAT;
        sum += error * error;
    }
    return sum / static_cast<double>(shifts.size());
}

FitResult fitWlf(const ExperimentInput& input) {
    const std::vector<ShiftPoint> shifts = deriveShiftPoints(input);
    if (shifts.size() < 3) {
        throw std::runtime_error("Need at least 3 shift points for WLF fit");
    }

    double aa = 0.0;
    double ab = 0.0;
    double bb = 0.0;
    double ar = 0.0;
    double br = 0.0;
    int informativeRows = 0;

    // Book approximation is isolated here:
    // log(a_T) = C1 * (T - T0) / (C2 + (T - T0)).
    // Rearranged for linear least squares by C1 and C2.
    for (const auto& p : shifts) {
        const double dt = p.temperature - input.baseTemperature;
        const double a = -dt;
        const double b = p.logAT;
        const double rhs = -p.logAT * dt;

        if (std::abs(a) < 1e-12 && std::abs(b) < 1e-12) {
            continue;
        }

        aa += a * a;
        ab += a * b;
        bb += b * b;
        ar += a * rhs;
        br += b * rhs;
        ++informativeRows;
    }

    if (informativeRows < 2) {
        throw std::runtime_error("Need at least 2 informative shift points besides the reference curve");
    }

    const double det = aa * bb - ab * ab;
    if (std::abs(det) < 1e-12) {
        throw std::runtime_error("Cannot fit WLF parameters: degenerate shift points");
    }

    FitResult out;
    out.c1 = (ar * bb - ab * br) / det;
    out.c2 = (aa * br - ab * ar) / det;
    out.rmse = std::sqrt(fitMse(shifts, input.baseTemperature, out.c1, out.c2));
    out.referenceCurveIndex = referenceCurveIndex(input);
    return out;
}

void writePlotSvg(const std::string& filePath, const ExperimentInput& input, const FitResult& fit) {
    const std::vector<ShiftPoint> shifts = deriveShiftPoints(input);
    const int refIndex = referenceCurveIndex(input);

    double minTime = std::numeric_limits<double>::infinity();
    double maxTime = -std::numeric_limits<double>::infinity();
    double minX = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double minShiftedX = std::numeric_limits<double>::infinity();
    double maxShiftedX = -std::numeric_limits<double>::infinity();
    double minEps = std::numeric_limits<double>::infinity();
    double maxEps = -std::numeric_limits<double>::infinity();

    for (const auto& curve : input.curves) {
        const auto points = toSemiLogPoints(curve);
        const auto shiftIt = std::find_if(shifts.begin(), shifts.end(), [&](const ShiftPoint& p) {
            return std::abs(p.temperature - curve.temperature) < 1e-9;
        });
        const double shift = (shiftIt == shifts.end()) ? 0.0 : shiftIt->logAT;
        for (const auto& p : points) {
            const double time = std::exp(p.x) / 3600.0;
            minTime = std::min(minTime, time);
            maxTime = std::max(maxTime, time);
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minShiftedX = std::min(minShiftedX, p.x + shift);
            maxShiftedX = std::max(maxShiftedX, p.x + shift);
            minEps = std::min(minEps, p.y);
            maxEps = std::max(maxEps, p.y);
        }
    }

    auto epsRange = paddedRange(minEps, maxEps);
    auto timeRange = paddedRange(minTime, maxTime);
    auto rawXRange = paddedRange(minX, maxX);
    auto shiftedXRange = paddedRange(minShiftedX, maxShiftedX);
    if (minTime >= 0.0 && maxTime <= 6.5 && minEps >= 0.0 && maxEps <= 12.5) {
        timeRange = {0.0, 6.0};
        epsRange = {0.0, 12.0};
    }

    double minT = shifts.front().temperature - input.baseTemperature;
    double maxT = shifts.back().temperature - input.baseTemperature;
    double minLogA = shifts.front().logAT;
    double maxLogA = shifts.front().logAT;
    double minResidual = 0.0;
    double maxResidual = 0.0;
    for (const auto& p : shifts) {
        minLogA = std::min(minLogA, p.logAT);
        maxLogA = std::max(maxLogA, p.logAT);
        const double model = modelLogAT(p.temperature, input.baseTemperature, fit.c1, fit.c2);
        const double residual = p.logAT - model;
        minResidual = std::min(minResidual, residual);
        maxResidual = std::max(maxResidual, residual);
    }

    std::vector<std::pair<double, double>> modelCurve;
    for (int i = 0; i <= 300; ++i) {
        const double dt = minT + (maxT - minT) * static_cast<double>(i) / 300.0;
        const double temperature = input.baseTemperature + dt;
        const double y = modelLogAT(temperature, input.baseTemperature, fit.c1, fit.c2);
        if (std::isfinite(y) && std::abs(fit.c2 + temperature - input.baseTemperature) > 1e-9) {
            modelCurve.push_back({dt, y});
        }
    }

    auto logARange = paddedRange(minLogA, maxLogA);
    auto residualRange = paddedRange(minResidual, maxResidual);

    std::ofstream out(std::filesystem::u8path(filePath));
    if (!out) {
        throw std::runtime_error("Cannot write plot SVG: " + filePath);
    }

    const PlotArea rawTimePlot{88.0, 92.0, 260.0, 260.0};
    const PlotArea rawLogPlot{430.0, 92.0, 260.0, 260.0};
    const PlotArea shiftedPlot{88.0, 440.0, 960.0, 210.0};
    const PlotArea shiftPlot{88.0, 760.0, 960.0, 140.0};

    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1120\" height=\"960\" viewBox=\"0 0 1120 960\">\n";
    out << "<style>"
           "text{font-family:Segoe UI,Arial,sans-serif;fill:#172033}.title{font-size:19px;font-weight:700}"
           ".subtitle{font-size:14px;fill:#4b5563}.axis-label{font-size:14px;font-weight:600;fill:#374151}"
           ".tick{font-size:12px;fill:#4b5563}.plot-bg{fill:#fbfcfe}.grid{stroke:#d8dee8;stroke-width:1}"
           ".frame{fill:none;stroke:#4b5563;stroke-width:1.2}.zero-line{stroke:#172033;stroke-width:1.4}"
           ".formula{fill:none;stroke:#111827;stroke-width:3}.point{stroke:#fff;stroke-width:1.2}.curve{fill:none;stroke-width:2.2}"
           ".stem{stroke:#9aa6b2;stroke-width:1.1}.badge{fill:#ffffff;stroke:#d8dee8;stroke-width:1}.legend{font-size:13px}"
           "</style>\n";
    out << "<defs><clipPath id=\"clip-shift-curve\"><rect x=\"" << svgNumber(shiftPlot.x)
        << "\" y=\"" << svgNumber(shiftPlot.y) << "\" width=\"" << svgNumber(shiftPlot.width)
        << "\" height=\"" << svgNumber(shiftPlot.height) << "\"/></clipPath></defs>\n";
    out << "<rect width=\"1120\" height=\"960\" fill=\"#ffffff\"/>\n";
    out << "<text x=\"48\" y=\"42\" class=\"title\">Experiment " << xmlEscape(input.experimentId)
        << ": creep curves, horizontal shifts, WLF approximation</text>\n";
    out << "<text x=\"48\" y=\"64\" class=\"subtitle\">T0=" << svgNumber(input.baseTemperature, 3)
        << ", reference curve T=" << svgNumber(input.curves[static_cast<std::size_t>(refIndex)].temperature, 3)
        << ", C1=" << svgNumber(fit.c1, 6) << ", C2=" << svgNumber(fit.c2, 6)
        << ", fit RMSE=" << svgNumber(fit.rmse, 6) << "</text>\n";

    writeSvgAxes(out, rawTimePlot, timeRange.first, timeRange.second, epsRange.first, epsRange.second,
                 "1a. Creep curves", "t", "epsilon");
    writeSvgAxes(out, rawLogPlot, rawXRange.first, rawXRange.second, epsRange.first, epsRange.second,
                 "1b. Semi-log creep curves", "ln(t, s)", "epsilon");
    writeSvgAxes(out, shiftedPlot, shiftedXRange.first, shiftedXRange.second, epsRange.first, epsRange.second,
                 "2. Master curve coordinates", "ln(t, s) + ln(aT)", "epsilon");
    writeSvgAxes(out, shiftPlot, minT, maxT, logARange.first, logARange.second,
                 "3. ln(aT) as a function of reduced temperature", "T - T0, C", "ln(aT)");

    struct SvgMasterPoint {
        double x{};
        double y{};
        std::size_t curveIndex{};
    };
    std::vector<SvgMasterPoint> masterPoints;

    for (std::size_t ci = 0; ci < input.curves.size(); ++ci) {
        const auto& curve = input.curves[ci];
        const auto points = toSemiLogPoints(curve);
        const std::string color = colorForIndex(ci);
        const auto shiftIt = std::find_if(shifts.begin(), shifts.end(), [&](const ShiftPoint& p) {
            return std::abs(p.temperature - curve.temperature) < 1e-9;
        });
        const double shift = (shiftIt == shifts.end()) ? 0.0 : shiftIt->logAT;

        out << "<polyline class=\"curve\" stroke=\"" << color << "\" points=\"";
        for (const auto& p : points) {
            const double time = std::exp(p.x) / 3600.0;
            out << svgNumber(mapX(time, rawTimePlot, timeRange.first, timeRange.second)) << ","
                << svgNumber(mapY(p.y, rawTimePlot, epsRange.first, epsRange.second)) << " ";
        }
        out << "\"><title>T=" << svgNumber(curve.temperature, 3) << "</title></polyline>\n";

        out << "<polyline class=\"curve\" stroke=\"" << color << "\" points=\"";
        for (const auto& p : points) {
            out << svgNumber(mapX(p.x, rawLogPlot, rawXRange.first, rawXRange.second)) << ","
                << svgNumber(mapY(p.y, rawLogPlot, epsRange.first, epsRange.second)) << " ";
        }
        out << "\"><title>T=" << svgNumber(curve.temperature, 3) << "</title></polyline>\n";

        for (const auto& p : points) {
            masterPoints.push_back({p.x + shift, p.y, ci});
        }
    }

    std::sort(masterPoints.begin(), masterPoints.end(), [](const SvgMasterPoint& left, const SvgMasterPoint& right) {
        return left.x < right.x;
    });
    out << "<polyline class=\"formula\" points=\"";
    for (const auto& p : masterPoints) {
        out << svgNumber(mapX(p.x, shiftedPlot, shiftedXRange.first, shiftedXRange.second)) << ","
            << svgNumber(mapY(p.y, shiftedPlot, epsRange.first, epsRange.second)) << " ";
    }
    out << "\"><title>Master curve from shifted experimental points</title></polyline>\n";
    for (const auto& p : masterPoints) {
        const std::string color = colorForIndex(p.curveIndex);
        out << "<circle class=\"point\" fill=\"" << color << "\" cx=\""
            << svgNumber(mapX(p.x, shiftedPlot, shiftedXRange.first, shiftedXRange.second)) << "\" cy=\""
            << svgNumber(mapY(p.y, shiftedPlot, epsRange.first, epsRange.second))
            << "\" r=\"4.2\"/>\n";
    }

    out << "<g clip-path=\"url(#clip-shift-curve)\"><polyline class=\"formula\" points=\"";
    for (const auto& p : modelCurve) {
        out << svgNumber(mapX(p.first, shiftPlot, minT, maxT)) << ","
            << svgNumber(mapY(p.second, shiftPlot, logARange.first, logARange.second)) << " ";
    }
    out << "\"/></g>\n";
    for (std::size_t i = 0; i < shifts.size(); ++i) {
        const auto& p = shifts[i];
        const std::string color = colorForIndex(i);
        out << "<circle class=\"point\" fill=\"" << color << "\" cx=\""
            << svgNumber(mapX(p.temperature - input.baseTemperature, shiftPlot, minT, maxT)) << "\" cy=\""
            << svgNumber(mapY(p.logAT, shiftPlot, logARange.first, logARange.second))
            << "\" r=\"5.5\"><title>T=" << svgNumber(p.temperature, 3)
            << ", ln(aT)=" << svgNumber(p.logAT, 6)
            << ", residual=" << svgNumber(p.logAT - modelLogAT(p.temperature, input.baseTemperature, fit.c1, fit.c2), 6)
            << "</title></circle>\n";
    }

    out << "</svg>\n";
}

#ifdef TVA_USE_SQLITE
static void execSqlite(sqlite3* db, const char* sql, bool ignoreDuplicateColumn = false) {
    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "unknown sqlite error";
        sqlite3_free(errMsg);
        if (ignoreDuplicateColumn && err.find("duplicate column name") != std::string::npos) {
            return;
        }
        throw std::runtime_error("sqlite exec failed: " + err);
    }
}

static void writeResultSqlite(const std::string& dbPath, const ExperimentInput& input, const FitResult& fit) {
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error("sqlite open failed: " + err);
    }

    try {
        execSqlite(db,
            "CREATE TABLE IF NOT EXISTS results ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "experiment_id TEXT NOT NULL,"
            "base_temperature REAL NOT NULL,"
            "reference_temperature REAL,"
            "c1 REAL NOT NULL,"
            "c2 REAL NOT NULL,"
            "rmse REAL NOT NULL,"
            "created_at TEXT DEFAULT CURRENT_TIMESTAMP"
            ");");
        execSqlite(db, "ALTER TABLE results ADD COLUMN reference_temperature REAL;", true);

        const char* insertSql =
            "INSERT INTO results (experiment_id, base_temperature, reference_temperature, c1, c2, rmse) "
            "VALUES (?, ?, ?, ?, ?, ?);";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::string err = sqlite3_errmsg(db);
            throw std::runtime_error("sqlite prepare failed: " + err);
        }

        const int refIndex = referenceCurveIndex(input);
        sqlite3_bind_text(stmt, 1, input.experimentId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, input.baseTemperature);
        sqlite3_bind_double(stmt, 3, input.curves[static_cast<std::size_t>(refIndex)].temperature);
        sqlite3_bind_double(stmt, 4, fit.c1);
        sqlite3_bind_double(stmt, 5, fit.c2);
        sqlite3_bind_double(stmt, 6, fit.rmse);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string err = sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            throw std::runtime_error("sqlite insert failed: " + err);
        }

        sqlite3_finalize(stmt);
        sqlite3_close(db);
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
}
#endif

static void writeResultFallback(const std::string& filePath, const ExperimentInput& input, const FitResult& fit) {
    const bool exists = std::filesystem::exists(std::filesystem::u8path(filePath));
    std::ofstream out(std::filesystem::u8path(filePath), std::ios::app);
    if (!out) {
        throw std::runtime_error("Cannot write fallback DB file: " + filePath);
    }
    if (!exists) {
        out << "experiment_id,base_temperature,reference_temperature,c1,c2,rmse\n";
    }
    const int refIndex = referenceCurveIndex(input);
    out << input.experimentId << ","
        << input.baseTemperature << ","
        << std::setprecision(12) << input.curves[static_cast<std::size_t>(refIndex)].temperature << ","
        << std::setprecision(12) << fit.c1 << ","
        << std::setprecision(12) << fit.c2 << ","
        << std::setprecision(12) << fit.rmse << "\n";
}

void writeResultDatabase(const std::string& dbPath, const ExperimentInput& input, const FitResult& fit) {
#ifdef TVA_USE_SQLITE
    writeResultSqlite(dbPath, input, fit);
#else
    writeResultFallback(dbPath + ".csv", input, fit);
#endif
}

bool sqliteStorageEnabled() {
#ifdef TVA_USE_SQLITE
    return true;
#else
    return false;
#endif
}

std::string resultStorageDescription(const std::string& dbPath) {
#ifdef TVA_USE_SQLITE
    return "Saved into SQLite DB: " + dbPath;
#else
    return "SQLite not found during build. Saved into fallback file DB: " + dbPath + ".csv";
#endif
}
