#include "tva_core.h"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: tva_fit <input.xml> [db_path] [plot_svg_path]\n";
            return 1;
        }

        const std::string xmlPath = argv[1];
        const std::string dbPath = (argc >= 3) ? argv[2] : "results.db";
        const std::string plotPath = (argc >= 4)
            ? argv[3]
            : (std::filesystem::u8path(xmlPath).replace_extension("").u8string() + "_plot.svg");

        const ExperimentInput input = parseExperimentXml(xmlPath);
        const FitResult fit = fitWlf(input);

        std::cout << std::fixed << std::setprecision(6);
        std::cout << "Experiment: " << input.experimentId << "\n";
        std::cout << "Base temperature (T0): " << input.baseTemperature << "\n";
        std::cout << "Creep curves: " << input.curves.size() << "\n";
        std::cout << "Horizontal shifts from curve matching:\n";
        for (const auto& point : deriveShiftPoints(input)) {
            std::cout << "  T=" << point.temperature
                      << "  ln(aT)=" << point.logAT
                      << "  curve_rmse=" << point.rmse
                      << "  overlap_points=" << point.overlapCount << "\n";
        }
        std::cout << "c1 = " << fit.c1 << "\n";
        std::cout << "c2 = " << fit.c2 << "\n";
        std::cout << "RMSE = " << fit.rmse << "\n";

        writeResultDatabase(dbPath, input, fit);
        std::cout << resultStorageDescription(dbPath) << "\n";

        writePlotSvg(plotPath, input, fit);
        std::cout << "Saved plot SVG: " << plotPath << "\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    }
}
