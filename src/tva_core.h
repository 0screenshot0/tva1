#pragma once

#include <string>
#include <vector>

struct CreepPoint {
    double time{};
    double epsilon{};
};

struct CreepCurve {
    double temperature{};
    std::vector<CreepPoint> points;
};

struct ShiftPoint {
    double temperature{};
    double logAT{};
    double rmse{};
    int overlapCount{};
};

struct ExperimentInput {
    std::string experimentId;
    double baseTemperature{};
    std::vector<CreepCurve> curves;
};

struct FitResult {
    double c1{};
    double c2{};
    double rmse{};
    int referenceCurveIndex{};
};

double semiLogTime(double timeHours);
std::string readTextFile(const std::string& path);
ExperimentInput parseExperimentXml(const std::string& xmlPath);
void writeExperimentXml(const std::string& filePath, const ExperimentInput& input);
std::vector<ShiftPoint> deriveShiftPoints(const ExperimentInput& input);
double modelLogAT(double temperature, double t0, double c1, double c2);
FitResult fitWlf(const ExperimentInput& input);
void writePlotSvg(const std::string& filePath, const ExperimentInput& input, const FitResult& fit);
void writeResultDatabase(const std::string& dbPath, const ExperimentInput& input, const FitResult& fit);
bool sqliteStorageEnabled();
std::string resultStorageDescription(const std::string& dbPath);
