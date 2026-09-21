# Epicrisis Deidentifier & Information Extractor

A C++17 pipeline designed to automatically deidentify sensitive medical documents and subsequently extract structured medical fields into multiple clean CSV files.

## Features
* **Automated Deidentification (Step 1):** Scans an input folder for supported files, detects sensitive personal data via custom detectors, and exports scrubbed versions to the Desktop (`files_for_deidentifying_deidentified`).
* **Structured Info Extraction (Step 2):** Parses the deidentified documents, extracts 27 clinical fields (e.g., diagnoses, anamnesis, therapy), and outputs well-formatted, multi-file CSV records (with UTF-8 BOM for Excel compatibility) to the Desktop (`epicrisies_info_extract`).
* **Modular Architecture:** Clean separation of concerns with dedicated modules for deidentification, file processing, detectors, metadata handling, and data extraction.

---

## Requirements & Dependencies
* **CMake** (version 3.20 or higher)
* **C++17** compatible compiler (AppleClang, GCC, or MSVC)
* **macOS / Linux** (Utilizes standard command-line tools like `textutil` on macOS for `.doc`/`.docx` file parsing)

---

## Building the Project

Open the terminal in the root project directory and run the following commands:

```bash
mkdir -p build
cd build
cmake ..
make
