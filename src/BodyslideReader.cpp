#include <SKSE/SKSE.h>
#include <BodyslideReader.h>
#include <rapidxml.hpp>
#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <string>
#include <vector>
#include <random>

using namespace rapidxml;
using namespace RE;
using namespace Primary;
using namespace SKSE;

const std::vector<std::string> DefaultSliders = {"Breasts",   "BreastsSmall", "NippleDistance", "NippleSize",
                                                 "ButtCrack", "Butt",         "ButtSmall",      "Legs",
                                                 "Arms",      "ShoulderWidth"};

PresetContainer* PresetContainer::GetInstance() {
    static PresetContainer instance;
    return std::addressof(instance);
}

namespace Presets {

    // Code in this namespace taken from AutoBodyAE and mostly unaltered, thank you!

    // today i learned about LNK2005.
    bodypreset notfound{};
    std::vector<slider> nullsliders;
    std::vector<bodypreset> vecNotFound;
    

    bodypreset FindPresetByName(std::vector<bodypreset> searchable, std::string name) {
        logger::trace("entered findpresetbyname");
        // logger::debug("the searchable set has {} elements in it.", searchable.size());
        for (bodypreset searchitem : searchable) {
            // logger::debug("the items name is {} and the search target is {}", searchitem.name, name);
            if (searchitem.name == name) {
                logger::trace("{} was found!", searchitem.name);
                return searchitem;
            }
        }
        // logger::info("Preset could not be found!");
        return notfound;
    }

    // everything to do with managing external files.
    namespace Parsing {
        // utility functions for morphs.ini parsing
        // https://stackoverflow.com/questions/12966957/
        std::vector<std::string> explode(std::string const& s, char delim) {
            std::vector<std::string> result;
            std::istringstream iss(s);

            for (std::string token; std::getline(iss, token, delim);) {
                result.push_back(std::move(token));
            }
            return result;
        }

        bool contains(std::vector<std::string> input, std::vector<std::string> comparisonitems) {
            for (std::string i : input) {
                for (std::string j : comparisonitems) {
                    if (i.find(j) != -1) {
                        return true;
                    }
                }
            }
            return false;
        }

        std::string seek(std::vector<std::string> input, std::vector<std::string> comparisonitems) {
            for (std::string i : input) {
                for (std::string j : comparisonitems) {
                    if (i.find(j) != -1) {
                        return i;
                    }
                }
            }
            return "";
        }

        void PrintPreset(bodypreset preset) {
            logger::info("Now displaying preset: {}", preset.name);
            for (slider bar : preset.sliderlist) {
                logger::info(" > {}: [Small: {}] [Big: {}]", bar.name, bar.min, bar.max);
            }
        }

        void PrintPresetList(std::vector<bodypreset> setofsets) {
            logger::trace("Trying to print the preset list.");
            for (bodypreset body : setofsets) {
                logger::info("[ {} ] Number of sliders: [ {} ]", body.name, body.sliderlist.size());
            }
        }

        // takes the list as a pointer
        void ParsePreset(std::string filename, std::vector<bodypreset>* femalelist, std::vector<bodypreset>* malelist) {
            logger::trace("Beginning xml parse...");

            // suck the xml into the buffer
            std::ifstream inputFile(filename);
            if (!inputFile.is_open()) {
                logger::warn("Failed to open preset file: {}", filename);
                return;
            }
            std::vector<char> buffer((std::istreambuf_iterator<char>(inputFile)), std::istreambuf_iterator<char>());
            buffer.push_back('\0');

            xml_document<> preset;
            try {
                // parse the buffer
                preset.parse<0>(&buffer[0]);
            } catch (const std::exception& e) {
                logger::warn("XML parse error in {}: {}", filename, e.what());
                return;
            }

            logger::trace("Buffer has been parsed.");
            //  find the root
            xml_node<>* root_node = preset.first_node("SliderPresets");
            if (!root_node) {
                logger::trace("Empty xml! Ignoring.");
                return;
            }
            // parse!
            // for each preset, grab its name and go through the sliders.
            for (xml_node<>* preset_node = root_node->first_node("Preset"); preset_node;
                 preset_node = preset_node->next_sibling("Preset")) {

                // we need this for the set of sets, to identify them.
                auto name_attr = preset_node->first_attribute("name");
                std::string presetname = name_attr ? name_attr->value() : (preset_node->first_attribute() ? preset_node->first_attribute()->value() : "");

                logger::trace("preset name being loaded is {}", presetname);
                std::vector<std::string> discardkeywords{"Cloth",  "cloth",    "Outfit", "outfit", "NeverNude",
                                                         "Bikini", "Feet",     "Hands",  "OUTFIT", "push",
                                                         "Push",   "Cleavage", "Armor",  "Bra"};
                // discard clothed presets.
                if (Presets::Parsing::contains({presetname}, discardkeywords)) {
                    logger::trace("Clothed preset found. Discarding.");
                    continue;
                }

                // fill the group vector with groups for THIS preset only to identify its sex.
                std::vector<std::string> presetgroups;
                for (xml_node<>* cat_node = preset_node->first_node("Group"); cat_node;
                     cat_node = cat_node->next_sibling("Group")) {
                    if (cat_node->first_attribute()) {
                        logger::trace("Adding {} to group list!", cat_node->first_attribute()->value());
                        presetgroups.push_back(cat_node->first_attribute()->value());
                    }
                }

                // for finding if we need to check for inverted sliders
                bool UUNP = false;
                auto set_attr = preset_node->first_attribute("set");
                if (set_attr && std::string(set_attr->value()).find("UNP") != std::string::npos) {
                    logger::trace("UUNP preset found via set attribute. Now inverting sliders.");
                    UUNP = true;
                } else {
                    for (const std::string& group : presetgroups) {
                        if (group.find("UNP") != std::string::npos) {
                            logger::trace("UUNP preset found via group. Now inverting sliders.");
                            UUNP = true;
                            break;
                        }
                    }
                }

                std::vector<slider> sliderset;
                std::string previousslidername = "";
                std::string lastSize = "";
                float lastsizevalue = 0.0f;

                // go through the sliders
                for (xml_node<>* slider_node = preset_node->first_node("SetSlider"); slider_node;
                     slider_node = slider_node->next_sibling("SetSlider")) {
                    auto slider_name_attr = slider_node->first_attribute("name");
                    std::string slidername = slider_name_attr ? slider_name_attr->value() : (slider_node->first_attribute() ? slider_node->first_attribute()->value() : "");

                    auto size_attr = slider_node->first_attribute("size");
                    std::string size = size_attr ? size_attr->value() : "";

                    auto val_attr = slider_node->first_attribute("value");
                    float sizevalue = 0.0f;
                    if (val_attr) {
                        try {
                            sizevalue = std::stoi(val_attr->value()) / 100.0f;
                        } catch (...) {
                            sizevalue = 0.0f;
                        }
                    }

                    // if we detect that a preset is UUNP based, invert the sliders.
                    bool inverted = false;
                    if (UUNP) {
                        for (const std::string& slider : DefaultSliders) {
                            if (slidername == slider) {
                                logger::trace("UUNP backwards slider found! Inverting...");
                                sizevalue = 1.0f - sizevalue;
                                inverted = true;
                                break;
                            }
                        }
                    }

                    // if a pair is found, push it into the sliderset vector as a full struct.
                    if (slidername == previousslidername) {
                        float lowVal = (lastSize == "small") ? lastsizevalue : sizevalue;
                        float highVal = (size == "big") ? sizevalue : lastsizevalue;
                        slider s;
                        s.name = slidername;
                        s.min = lowVal;
                        s.max = highVal;
                        sliderset.push_back(s);
                    }
                    // if a pair is not found, evaluate which half it is and then push it as a
                    // full struct with default values where they belong.
                    else if (slidername != previousslidername) {
                        xml_node<>* next_node = slider_node->next_sibling("SetSlider");
                        std::string next_name = "";
                        if (next_node) {
                            auto next_name_attr = next_node->first_attribute("name");
                            next_name = next_name_attr ? next_name_attr->value() : (next_node->first_attribute() ? next_node->first_attribute()->value() : "");
                        }

                        if (next_name != slidername) {
                            float defaultvalue = inverted ? 1.0f : 0.0f;
                            slider s;
                            s.name = slidername;
                            if (size == "big") {
                                s.min = defaultvalue;
                                s.max = sizevalue;
                            } else {
                                s.min = sizevalue;
                                s.max = defaultvalue;
                            }
                            sliderset.push_back(s);
                        }
                    }

                    previousslidername = slidername;
                    lastSize = size;
                    lastsizevalue = sizevalue;
                }

                // push the preset into the master list based on groups for THIS preset
                bool fail = true;
                for (const std::string& item : presetgroups) {
                    if (item.find("CBBE") != std::string::npos || item.find("3BBB") != std::string::npos ||
                        item.find("CBAdvanced") != std::string::npos || item.find("UNP") != std::string::npos ||
                        item.find("TBD") != std::string::npos) {
                        logger::info("{} has been added to the female preset list.", presetname);
                        fail = false;
                        femalelist->push_back(bodypreset{sliderset, presetname});
                        break;
                    } else if (item.find("HIMBO") != std::string::npos || item.find("SOS") != std::string::npos ||
                               item.find("SAM") != std::string::npos) {
                        logger::info("{} has been added to the male preset list.", presetname);
                        fail = false;
                        malelist->push_back(bodypreset{sliderset, presetname});
                        break;
                    }
                }
                if (fail) {
                    logger::info("{} is not assigned to any supported preset groups! It will not be loaded.",
                                 presetname);
                }
            }

            return;
        }

        // finds all the xmls in a target folder, and parses them into an xml vector.
        void ParseAllInFolder(std::string path, std::vector<bodypreset>* femalelist,
                              std::vector<bodypreset>* malelist) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec) || ec) {
                logger::warn("Directory not found or inaccessible: {}", path);
                return;
            }

            for (auto const& dir_entry : std::filesystem::directory_iterator(path, ec)) {
                if (ec) {
                    logger::warn("Error reading directory: {}", ec.message());
                    break;
                }
                if (dir_entry.is_regular_file(ec) && dir_entry.path().extension() == ".xml") {
                    std::string filepath = dir_entry.path().string();
                    logger::trace("Now parsing {}...", dir_entry.path().filename().string());
                    ParsePreset(filepath, femalelist, malelist);
                }
            }

            return;
        }

        std::string CheckConfig() {
            // we return this at the end to help the parser find the path.
            std::string presetPath;

            presetPath = "Data\\CalienteTools\\BodySlide\\SliderPresets";
 

            return presetPath;
        }

    }  // namespace Parsing

}  // namespace Presets

BodyslideReader& BodyslideReader::GetSingleton() noexcept {
    static BodyslideReader instance;
    return instance;
}

void BodyslideReader::ReloadPresets() {
    std::unique_lock lock(_lock);
    auto presetcontainer = PresetContainer::GetInstance();
    std::vector<bodypreset> newMasterSet;
    presetcontainer->femaleMasterSet = newMasterSet;
    presetcontainer->maleMasterSet = newMasterSet;
    //logger::info("female master size: {}", presetcontainer->femaleMasterSet.size());
    Presets::Parsing::ParseAllInFolder(Presets::Parsing::CheckConfig(), &presetcontainer->femaleMasterSet,
                                       &presetcontainer->maleMasterSet);
    logger::info("ReloadPresets {} presets were loaded into the female list.", presetcontainer->femaleMasterSet.size());
    logger::info("ReloadPresets {} presets were loaded into the male list.", presetcontainer->maleMasterSet.size());
        return;
}

std::vector<std::float_t> BodyslideReader::GetPresetSliderLows(std::string name) {
        std::unique_lock lock(_lock);

        auto presetcontainer = PresetContainer::GetInstance();

        logger::info("GetPresetSliderLows called and grabbed container.");
        std::vector<float_t> returnarray;

        for (int i = 0; i < presetcontainer->femaleMasterSet.size(); ++i) {
        if (presetcontainer->femaleMasterSet[i].name == name) {
            logger::info("GetPresetSliderLows Passed female compare!");
            const bodypreset& preset = (presetcontainer->femaleMasterSet[i]);
            for (const slider& bar : preset.sliderlist) {
                returnarray.push_back(bar.min);
            }
            return returnarray;
            }
        }
         
        for (int i = 0; i < presetcontainer->maleMasterSet.size(); ++i) {
        if (presetcontainer->maleMasterSet[i].name == name) {
            logger::info("GetPresetSliderLows Passed male compare!");
            const bodypreset& preset = (presetcontainer->maleMasterSet[i]);
            for (const slider& bar : preset.sliderlist) {
                returnarray.push_back(bar.min);
            }
            return returnarray;
        }  
        }
        
        logger::info("GetPresetSliderLows hit failsafe return. No data found.");
        return returnarray;
}

std::vector<std::float_t> BodyslideReader::GetPresetSliderHighs(std::string name) {
        std::unique_lock lock(_lock);

        auto presetcontainer = PresetContainer::GetInstance();

        logger::info("GetPresetSliderHighs called and grabbed container.");
        std::vector<float_t> returnarray;

        for (int i = 0; i < presetcontainer->femaleMasterSet.size(); ++i) {
        if (presetcontainer->femaleMasterSet[i].name == name) {
            logger::info("GetPresetSliderHighs Passed female compare!");
            const bodypreset& preset = (presetcontainer->femaleMasterSet[i]);
            for (const slider& bar : preset.sliderlist) {
                returnarray.push_back(bar.max);
            }
            return returnarray;
        }
        }
        
        for (int i = 0; i < presetcontainer->maleMasterSet.size(); ++i) {
        if (presetcontainer->maleMasterSet[i].name == name) {
            logger::info("GetPresetSliderHighs Passed male compare!");
            const bodypreset& preset = (presetcontainer->maleMasterSet[i]);
            for (const slider& bar : preset.sliderlist) {
                returnarray.push_back(bar.max);
            }
            return returnarray;
        }
        }
        logger::info("GetPresetSliderHighs hit failsafe return. No data found.");
        return returnarray;
}

std::vector<std::string> BodyslideReader::GetPresetSliderStrings(std::string name) {
        std::unique_lock lock(_lock);

        auto presetcontainer = PresetContainer::GetInstance();

        logger::info("GetPresetSliderStrings called and grabbed container.");
        std::vector<std::string> returnarray;

        for (int i = 0; i < presetcontainer->femaleMasterSet.size(); ++i) {
        if (presetcontainer->femaleMasterSet[i].name == name) {
            logger::info("GetPresetSliderStrings Passed female compare!");
            const bodypreset& preset = (presetcontainer->femaleMasterSet[i]);
            for (const slider& bar : preset.sliderlist) {
                returnarray.push_back(bar.name);
            }
            return returnarray;
        }
        }

        for (int i = 0; i < presetcontainer->maleMasterSet.size(); ++i) {
        if (presetcontainer->maleMasterSet[i].name == name) {
            logger::info("GetPresetSliderStrings Passed male compare!");
            const bodypreset& preset = (presetcontainer->maleMasterSet[i]);
            for (const slider& bar : preset.sliderlist) {
                returnarray.push_back(bar.name);
            }
            return returnarray;
        }
        }

        logger::info("GetPresetSliderStrings hit failsafe return. No data found.");
        return returnarray;
}

std::vector<std::string> BodyslideReader::GetPresetList(bool female) { 
    std::unique_lock lock(_lock);

    auto presetcontainer = PresetContainer::GetInstance();

    logger::info("GetPresetList called and grabbed container.");
    std::vector<std::string> returnarray;
    if (female == true) {
        logger::info("GetPresetList Passed female bool!");
        for (int i = 0; i < presetcontainer->femaleMasterSet.size(); ++i) {
            bodypreset preset = (presetcontainer->femaleMasterSet[i]);
            returnarray.push_back(preset.name);
        }
        //logger::info("GetPresetList Passed female For with array of size: {}", returnarray.size());
    } 
    else if (female == false) {
        logger::info("GetPresetList Passed male bool!");
        for (int i = 0; i < presetcontainer->maleMasterSet.size(); ++i) {
            bodypreset preset = (presetcontainer->maleMasterSet[i]);
            returnarray.push_back(preset.name);
        }
        //logger::info("GetPresetList Passed male For with array of size: {}", returnarray.size());
    }
    logger::info("GetPresetList bool: {} returning an array with {} presets.", female, returnarray.size());
    return returnarray;

}