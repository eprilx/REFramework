#include <chrono>
#include <filesystem>
#include <fstream>

#include <json.hpp>
#include <utility/String.hpp>
#include <imgui.h>

#include "OpenXR.hpp"

using namespace nlohmann;

namespace runtimes {
VRRuntime::Error OpenXR::synchronize_frame() {
    std::scoped_lock _{sync_mtx};

    // cant sync frame between begin and endframe
    if (!this->session_ready || this->frame_began) {
        return VRRuntime::Error::UNSPECIFIED;
    }

    if (this->frame_synced) {
        return VRRuntime::Error::SUCCESS;
    }

    this->begin_profile();

    XrFrameWaitInfo frame_wait_info{XR_TYPE_FRAME_WAIT_INFO};
    this->frame_state = {XR_TYPE_FRAME_STATE};
    auto result = xrWaitFrame(this->session, &frame_wait_info, &this->frame_state);

    this->end_profile("xrWaitFrame");

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrWaitFrame failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    } else {
        this->got_first_sync = true;
        this->frame_synced = true;
    }

    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_poses() {
    std::scoped_lock _{ this->sync_mtx };
    std::unique_lock __{ this->pose_mtx };

    if (!this->session_ready) {
        return VRRuntime::Error::SUCCESS;
    }

    /*if (!this->needs_pose_update) {
        return VRRuntime::Error::SUCCESS;
    }*/

    this->view_state = {XR_TYPE_VIEW_STATE};
    this->stage_view_state = {XR_TYPE_VIEW_STATE};

    uint32_t view_count{};

    const auto display_time = this->frame_state.predictedDisplayTime + (this->frame_state.predictedDisplayPeriod * this->prediction_scale);

    if (display_time == 0) {
        return VRRuntime::Error::SUCCESS;
    }

    XrViewLocateInfo view_locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    view_locate_info.viewConfigurationType = this->view_config;
    view_locate_info.displayTime = display_time;
    view_locate_info.space = this->view_space;

    auto result = xrLocateViews(this->session, &view_locate_info, &this->view_state, (uint32_t)this->views.size(), &view_count, this->views.data());

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateViews for view space failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    view_locate_info = {XR_TYPE_VIEW_LOCATE_INFO};
    view_locate_info.viewConfigurationType = this->view_config;
    view_locate_info.displayTime = display_time;
    view_locate_info.space = this->stage_space;

    result = xrLocateViews(this->session, &view_locate_info, &this->stage_view_state, (uint32_t)this->stage_views.size(), &view_count, this->stage_views.data());

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateViews for stage space failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    result = xrLocateSpace(this->view_space, this->stage_space, display_time, &this->view_space_location);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateSpace for view space failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    for (auto& hand : this->hands) {
        hand.location.next = &hand.velocity;
        result = xrLocateSpace(hand.space, this->stage_space, display_time, &hand.location);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrLocateSpace for hand space failed: {}", this->get_result_string(result));
            return (VRRuntime::Error)result;
        }
    }

    this->needs_pose_update = false;
    this->got_first_poses = true;
    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_render_target_size() {
    uint32_t view_count{};
    auto result = xrEnumerateViewConfigurationViews(this->instance, this->system, this->view_config, 0, &view_count, nullptr); 
    if (result != XR_SUCCESS) {
        this->error = "Could not get view configuration properties: " + this->get_result_string(result);
        spdlog::error("[VR] {}", this->error.value());

        return (VRRuntime::Error)result;
    }

    this->view_configs.resize(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result = xrEnumerateViewConfigurationViews(this->instance, this->system, this->view_config, view_count, &view_count, this->view_configs.data());
    if (result != XR_SUCCESS) {
        this->error = "Could not get view configuration properties: " + this->get_result_string(result);
        spdlog::error("[VR] {}", this->error.value());

        return (VRRuntime::Error)result;
    }

    return VRRuntime::Error::SUCCESS;
}

uint32_t OpenXR::get_width() const {
    if (this->view_configs.empty()) {
        return 0;
    }

    return (uint32_t)((float)this->view_configs[0].recommendedImageRectWidth * this->resolution_scale);
}

uint32_t OpenXR::get_height() const {
    if (this->view_configs.empty()) {
        return 0;
    }

    return (uint32_t)((float)this->view_configs[0].recommendedImageRectHeight * this->resolution_scale);
}

VRRuntime::Error OpenXR::consume_events(std::function<void(void*)> callback) {
    std::scoped_lock _{sync_mtx};

    XrEventDataBuffer edb{XR_TYPE_EVENT_DATA_BUFFER};
    auto result = xrPollEvent(this->instance, &edb);

    const auto bh = (XrEventDataBaseHeader*)&edb;

    while (result == XR_SUCCESS) {
        spdlog::info("VR: xrEvent: {}", this->get_structure_string(bh->type));

        if (callback) {
            callback(&edb);
        }

        if (bh->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto ev = (XrEventDataSessionStateChanged*)&edb;
            this->session_state = ev->state;

            spdlog::info("VR: XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED {}", ev->state);

            if (ev->state == XR_SESSION_STATE_READY) {
                spdlog::info("VR: XR_SESSION_STATE_READY");
                
                // Begin the session
                XrSessionBeginInfo session_begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                session_begin_info.primaryViewConfigurationType = this->view_config;

                result = xrBeginSession(this->session, &session_begin_info);

                if (result != XR_SUCCESS) {
                    this->error = std::string{"xrBeginSessionFailed: "} + this->get_result_string(result);
                    spdlog::error("VR: xrBeginSession failed: {}", this->get_result_string(result));
                } else {
                    this->session_ready = true;
                    synchronize_frame();
                }
            } else if (ev->state == XR_SESSION_STATE_LOSS_PENDING) {
                spdlog::info("VR: XR_SESSION_STATE_LOSS_PENDING");
                this->wants_reinitialize = true;
            } else if (ev->state == XR_SESSION_STATE_STOPPING) {
                spdlog::info("VR: XR_SESSION_STATE_STOPPING");

                if (this->ready()) {
                    xrEndSession(this->session);
                    this->session_ready = false;
                    this->frame_synced = false;
                    this->frame_began = false;

                    if (this->wants_reinitialize) {
                        //initialize_openxr();
                    }
                }
            }
        }

        edb = {XR_TYPE_EVENT_DATA_BUFFER};
        result = xrPollEvent(this->instance, &edb);
    } 
    
    if (result != XR_EVENT_UNAVAILABLE) {
        spdlog::error("VR: xrPollEvent failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_matrices(float nearz, float farz) {
    if (!this->session_ready || this->views.empty()) {
        return VRRuntime::Error::SUCCESS;
    }

    std::unique_lock __{ this->eyes_mtx };
    std::unique_lock ___{ this->pose_mtx };

    for (auto i = 0; i < 2; ++i) {
        const auto& pose = this->views[i].pose;
        const auto& fov = this->views[i].fov;

        // Update projection matrix
        XrMatrix4x4f_CreateProjection((XrMatrix4x4f*)&this->projections[i], GRAPHICS_D3D, tan(fov.angleLeft), tan(fov.angleRight), tan(fov.angleUp), tan(fov.angleDown), nearz, farz);

        // Update view matrix
        this->eyes[i] = Matrix4x4f{*(glm::quat*)&pose.orientation};
        this->eyes[i][3] = Vector4f{*(Vector3f*)&pose.position, 1.0f};
    }

    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_input() {
    if (!this->ready() || this->session_state != XR_SESSION_STATE_FOCUSED) {
        return (VRRuntime::Error)XR_ERROR_SESSION_NOT_READY;
    }

    XrActiveActionSet active_action_set{this->action_set.handle, XR_NULL_PATH};
    XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
    sync_info.countActiveActionSets = 1;
    sync_info.activeActionSets = &active_action_set;

    auto result = xrSyncActions(this->session, &sync_info);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to sync actions: {}", this->get_result_string(result));

        return (VRRuntime::Error)result;
    }

    for (auto i = 0; i < 2; ++i) {
        if (this->is_action_active_once("systembutton", (VRRuntime::Hand)i)) {
            this->handle_pause = true;
        }

        // Update controller pose state
        XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
        get_info.subactionPath = this->hands[i].path;
        get_info.action = this->action_set.action_map["pose"];

        XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};

        result = xrGetActionStatePose(this->session, &get_info, &pose_state);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to get action state pose {}: {}", i, this->get_result_string(result));

            return (VRRuntime::Error)result;
        }

        this->hands[i].active = pose_state.isActive;
        // TODO: Other hand inputs
    }

    // TODO: Other non-hand specific inputs
    return VRRuntime::Error::SUCCESS;
}

void OpenXR::destroy() {
    if (!this->loaded) {
        return;
    }

    std::scoped_lock _{sync_mtx};

    if (this->session != nullptr) {
        if (this->session_ready) {
            xrEndSession(this->session);
        }

        xrDestroySession(this->session);
    }

    if (this->instance != nullptr) {
        xrDestroyInstance(this->instance);
        this->instance = nullptr;
    }

    this->session = nullptr;
    this->session_ready = false;
    this->system = XR_NULL_SYSTEM_ID;
    this->frame_synced = false;
    this->frame_began = false;
}

std::string OpenXR::get_result_string(XrResult result) const {
    std::string result_string{};
    result_string.resize(XR_MAX_RESULT_STRING_SIZE);
    xrResultToString(this->instance, result, result_string.data());

    return result_string;
}

std::string OpenXR::get_structure_string(XrStructureType type) const {
    std::string structure_string{};
    structure_string.resize(XR_MAX_STRUCTURE_NAME_SIZE);
    xrStructureTypeToString(this->instance, type, structure_string.data());

    return structure_string;
}

std::string OpenXR::get_path_string(XrPath path) const {
    if (path == XR_NULL_PATH) {
        return "XR_NULL_PATH";
    }

    std::string path_string{};
    path_string.resize(XR_MAX_PATH_LENGTH);

    uint32_t real_size{};

    if (auto result = xrPathToString(this->instance, path, XR_MAX_PATH_LENGTH, &real_size, path_string.data()); result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get path string: {}", this->get_result_string(result));
        return "";
    }

    path_string.resize(real_size-1);
    return path_string;
}

std::string OpenXR::get_current_interaction_profile() const {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(this->session, this->hands[0].path, &state) != XR_SUCCESS) {
        return "";
    }

    return this->get_path_string(state.interactionProfile);
}

XrPath OpenXR::get_current_interaction_profile_path() const {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(this->session, this->hands[0].path, &state) != XR_SUCCESS) {
        return XR_NULL_PATH;
    }

    return state.interactionProfile;
}

std::optional<std::string> OpenXR::initialize_actions(const std::string& json_string) {
    spdlog::info("[VR] Initializing actions");

    if (auto result = xrStringToPath(this->instance, "/user/hand/left", &this->hands[VRRuntime::Hand::LEFT].path); result != XR_SUCCESS) {
        return "xrStringToPath failed (left): " + this->get_result_string(result);
    }

    if (auto result = xrStringToPath(this->instance, "/user/hand/right", &this->hands[VRRuntime::Hand::RIGHT].path); result != XR_SUCCESS) {
        return "xrStringToPath failed (right): " + this->get_result_string(result);
    }

    std::array<XrPath, 2> hand_paths{
        this->hands[VRRuntime::Hand::LEFT].path, 
        this->hands[VRRuntime::Hand::RIGHT].path
    };

    if (json_string.empty()) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating action set");

    XrActionSetCreateInfo action_set_create_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(action_set_create_info.actionSetName, "defaultopenxr");
    strcpy(action_set_create_info.localizedActionSetName, "Default");
    action_set_create_info.priority = 0;

    if (auto result = xrCreateActionSet(this->instance, &action_set_create_info, &this->action_set.handle); result != XR_SUCCESS) {
        return "xrCreateActionSet failed: " + this->get_result_string(result);
    }

    // Parse the JSON string using nlohmann
    json actions_json{};

    try {
        actions_json = json::parse(json_string);
    } catch (const std::exception& e) {
        return std::string{"json parse failed: "} + e.what();
    }

    if (actions_json.count("actions") == 0) {
        return "json missing actions";
    }

    auto actions_list = actions_json["actions"];
    bool has_pose_action = false;

    std::unordered_map<std::string, std::vector<XrActionSuggestedBinding>> profile_bindings{};

    for (const auto& controller : s_supported_controllers) {
        profile_bindings[controller] = {};
    }

    auto attempt_add_binding = [&](const std::string& interaction_profile, const XrActionSuggestedBinding& binding) -> bool {
        XrPath interaction_profile_path{};
        auto result = xrStringToPath(this->instance, interaction_profile.c_str(), &interaction_profile_path);
        auto& bindings = profile_bindings[interaction_profile];

        if (result == XR_SUCCESS) {
            bindings.push_back(binding);

            XrInteractionProfileSuggestedBinding suggested_bindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested_bindings.interactionProfile = interaction_profile_path;
            suggested_bindings.countSuggestedBindings = (uint32_t)bindings.size();
            suggested_bindings.suggestedBindings = bindings.data();

            result = xrSuggestInteractionProfileBindings(this->instance, &suggested_bindings);

            if (result != XR_SUCCESS) {
                bindings.pop_back();
                spdlog::info("Bad binding passed to xrSuggestInteractionProfileBindings from {}: {}", interaction_profile, this->get_result_string(result));
                return false;
            }

            return true;
        } else {
            spdlog::info("Bad interaction profile passed to xrStringToPath: {}", this->get_result_string(result));
            return false;
        }
    };

    for (auto& action : actions_list) {
        XrActionCreateInfo action_create_info{XR_TYPE_ACTION_CREATE_INFO};
        auto action_name = action["name"].get<std::string>();

        if (auto it = action_name.find_last_of("/"); it != std::string::npos) {
            action_name = action_name.substr(it + 1);
        }

        auto localized_action_name = action_name;
        std::transform(action_name.begin(), action_name.end(), action_name.begin(), ::tolower);

        strcpy(action_create_info.actionName, action_name.c_str());
        strcpy(action_create_info.localizedActionName, localized_action_name.c_str());

        action_create_info.countSubactionPaths = (uint32_t)hand_paths.size();
        action_create_info.subactionPaths = hand_paths.data();

        if (action_name == "pose") {
            has_pose_action = true;
        }

        std::unordered_set<XrAction>* out_actions = nullptr;

        // Translate the OpenVR action types to OpenXR action types
        switch (utility::hash(action["type"].get<std::string>())) {
            case "boolean"_fnv:
                if (action["type"].get<std::string>().ends_with("/value")) {
                    action_create_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
                    out_actions = &this->action_set.float_actions;
                } else {
                    action_create_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
                    out_actions = &this->action_set.bool_actions;
                }
                
                break;
            case "skeleton"_fnv: // idk what this is in OpenXR
                continue;
            case "pose"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
                out_actions = &this->action_set.pose_actions;
                break;
            case "vector1"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
                out_actions = &this->action_set.float_actions;
                break;
            case "vector2"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
                out_actions = &this->action_set.vector2_actions;
                break;
            case "vibration"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
                out_actions = &this->action_set.vibration_actions;
                break;
            default:
                continue;
        }
        
        // Create the action
        XrAction xr_action{XR_NULL_HANDLE};
        if (auto result = xrCreateAction(this->action_set.handle, &action_create_info, &xr_action); result != XR_SUCCESS) {
            return "xrCreateAction failed for " + action_name + ": " + this->get_result_string(result);
        }

        if (out_actions != nullptr) {
            out_actions->insert(xr_action);
        }

        spdlog::info("[VR] Created action {} with handle {:x}", action_name, (uintptr_t)xr_action);

        this->action_set.actions.push_back(xr_action);
        this->action_set.action_map[action_name] = xr_action;
        this->action_set.action_names[xr_action] = action_name;

        // Suggest bindings
        for (const auto& map_it : OpenXR::s_bindings_map) {
            if (map_it.action_name != action_name) {
                continue;
            }

            const auto& interaction_string = map_it.interaction_path_name;

            for (auto i = 0; i < 2; ++i) {
                auto hand_string = interaction_string;
                auto it = hand_string.find('*');
                auto index = i;
                bool wildcard = false;

                if (it != std::string::npos) {
                    if (i == VRRuntime::Hand::LEFT) {
                        hand_string.erase(it, 1);
                        hand_string.insert(it, "left");
                    } else if (i == VRRuntime::Hand::RIGHT) {
                        hand_string.erase(it, 1);
                        hand_string.insert(it, "right");
                    }

                    wildcard = true;
                } else {
                    if (hand_string.find("left") != std::string::npos) {
                        index = VRRuntime::Hand::LEFT;
                    } else if (hand_string.find("right") != std::string::npos) {
                        index = VRRuntime::Hand::RIGHT;
                    }
                }

                spdlog::info("[VR] {}", hand_string);

                XrPath p{XR_NULL_PATH};
                auto result = xrStringToPath(this->instance, hand_string.c_str(), &p);

                if (result != XR_SUCCESS || p == XR_NULL_PATH) {
                    spdlog::error("[VR] Failed to find path for {}", hand_string);

                    if (!wildcard) {
                        break;
                    }

                    continue;
                }

                if (this->action_set.action_map.contains(map_it.action_name)) {
                    for (const auto& controller : s_supported_controllers) {
                        if (attempt_add_binding(controller, { this->action_set.action_map[map_it.action_name], p })) {
                            this->hands[index].path_map[controller][map_it.action_name] = p;
                        }
                    }
                }

                if (!wildcard) {
                    break;
                }
            }
        }
    }

    if (!has_pose_action) {
        return "json missing pose action";
    }

    // Check for json files that will override the default suggested bindings
    for (const auto& controller : s_supported_controllers) {
        auto filename = controller + ".json";

        // replace the slashes with underscores
        std::replace(filename.begin(), filename.end(), '/', '_');

        // check if the file exists
        if (std::filesystem::exists(filename)) {
            spdlog::info("[VR] Loading bindings for {}", filename);

            profile_bindings[controller].clear();

            this->hands[VRRuntime::Hand::LEFT].path_map[controller].clear();
            this->hands[VRRuntime::Hand::RIGHT].path_map[controller].clear();

            // load the json file
            auto j = nlohmann::json::parse(std::ifstream(filename));

            for (auto it : j["bindings"]) {
                auto action_str = it["action"].get<std::string>();
                auto path_str = it["path"].get<std::string>();

                XrPath p{XR_NULL_PATH};
                auto result = xrStringToPath(this->instance, path_str.c_str(), &p);

                if (result != XR_SUCCESS || p == XR_NULL_PATH) {
                    spdlog::error("[VR] Failed to find path for {}", path_str);
                    continue;
                }

                const auto hand_idx = path_str.find("/left/") != std::string::npos ? VRRuntime::Hand::LEFT : VRRuntime::Hand::RIGHT;

                if (this->action_set.action_map.contains(action_str)) {
                    for (const auto& controller : s_supported_controllers) {
                        if (attempt_add_binding(controller, { this->action_set.action_map[action_str], p })) {
                            this->hands[hand_idx].path_map[controller][action_str] = p;
                        }
                    }
                }
            }
        }
    }

    // Create the action spaces for each hand
    for (auto i = 0; i < 2; ++i) {
        spdlog::info("[VR] Creating action space for hand {}", i);
        
        XrActionSpaceCreateInfo action_space_create_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        action_space_create_info.action = this->action_set.action_map["pose"];
        action_space_create_info.subactionPath = this->hands[i].path;
        action_space_create_info.poseInActionSpace.orientation.w = 1.0f;

        if (auto result = xrCreateActionSpace(this->session, &action_space_create_info, &this->hands[i].space); result != XR_SUCCESS) {
            return "xrCreateActionSpace failed (" + std::to_string(i) + ")" + this->get_result_string(result);
        }
    }

    // Attach the action set to the session
    spdlog::info("[VR] Attaching action set to session");

    XrSessionActionSetsAttachInfo action_sets_attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    action_sets_attach_info.countActionSets = 1;
    action_sets_attach_info.actionSets = &this->action_set.handle;

    if (auto result = xrAttachSessionActionSets(this->session, &action_sets_attach_info); result != XR_SUCCESS) {
        return "xrAttachSessionActionSets failed: " + this->get_result_string(result);
    }

    return std::nullopt;
}

bool OpenXR::is_action_active(XrAction action, VRRuntime::Hand hand) const {
    if (hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;
    
    if (this->action_set.bool_actions.contains(action)) {
        XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
        auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to get action state: {}", this->get_result_string(result));
            return false;
        }

        return active.isActive == XR_TRUE && active.currentState == XR_TRUE;
    } else if (this->action_set.float_actions.contains(action)) {
        XrActionStateFloat active{XR_TYPE_ACTION_STATE_FLOAT};
        auto result = xrGetActionStateFloat(this->session, &get_info, &active);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to get action state: {}", this->get_result_string(result));
            return false;
        }

        return active.isActive == XR_TRUE && active.currentState > 0.0f;
    } // idk?

    return false;
}

bool OpenXR::is_action_active(std::string_view action_name, VRRuntime::Hand hand) const {
    if (!this->action_set.action_map.contains(action_name.data()) || hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = this->action_set.action_map.find(action_name.data())->second;
    get_info.subactionPath = this->hands[hand].path;
    
    XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
    auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get action state for {}: {}", action_name, this->get_result_string(result));
        return false;
    }

    return active.isActive == XR_TRUE && active.currentState == XR_TRUE;
}

bool OpenXR::is_action_active_once(std::string_view action_name, VRRuntime::Hand hand) const {
    if (!this->action_set.action_map.contains(action_name.data()) || hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = this->action_set.action_map.find(action_name.data())->second;
    get_info.subactionPath = this->hands[hand].path;
    
    XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
    auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get action state for {}: {}", action_name, this->get_result_string(result));
        return false;
    }

    return active.isActive == XR_TRUE && active.currentState == XR_TRUE && active.changedSinceLastSync == XR_TRUE;
}

std::string OpenXR::translate_openvr_action_name(std::string action_name) const {
    if (action_name.empty()) {
        return action_name;
    }

    if (auto it = action_name.find_last_of("/"); it != std::string::npos) {
        action_name = action_name.substr(it + 1);
    }

    std::transform(action_name.begin(), action_name.end(), action_name.begin(), ::tolower);
    return action_name;
}

Vector2f OpenXR::get_left_stick_axis() const {
    if (!this->action_set.action_map.contains("joystick")) {
        return Vector2f{};
    }

    auto action = this->action_set.action_map.find("joystick")->second;

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = this->action_set.action_map.find("joystick")->second;
    get_info.subactionPath = this->hands[VRRuntime::Hand::LEFT].path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

Vector2f OpenXR::get_right_stick_axis() const {
    if (!this->action_set.action_map.contains("joystick")) {
        return Vector2f{};
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = this->action_set.action_map.find("joystick")->second;
    get_info.subactionPath = this->hands[VRRuntime::Hand::RIGHT].path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

void OpenXR::trigger_haptic_vibration(float duration, float frequency, float amplitude, VRRuntime::Hand source) const {
    if (!this->action_set.action_map.contains("haptic")) {
        return;
    }

    XrHapticActionInfo haptic_info{XR_TYPE_HAPTIC_ACTION_INFO};
    haptic_info.action = this->action_set.action_map.find("haptic")->second;
    haptic_info.subactionPath = this->hands[source].path;

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    vibration.frequency = frequency;

    // cast the duration from seconds to nanoseconds
    vibration.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<float>(duration)).count();

    auto result = xrApplyHapticFeedback(this->session, &haptic_info, (XrHapticBaseHeader*)&vibration);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to apply haptic feedback: {}", this->get_result_string(result));
    }
}

void OpenXR::display_bindings_editor() {
    const auto current_interaction_profile = this->get_current_interaction_profile();
    ImGui::Text("Interaction Profile: %s", current_interaction_profile.c_str());

    if (ImGui::Button("Restore Default Bindings")) {
        auto filename = current_interaction_profile + ".json";
        
        // replace the slashes with underscores
        std::replace(filename.begin(), filename.end(), '/', '_');

        if (std::filesystem::exists(filename)) {
            // Delete the file
            std::filesystem::remove(filename);
            this->wants_reinitialize = true;
        }
    }

    auto display_hand = [&](const std::string& name, uint32_t index) {
        if (current_interaction_profile.empty() || current_interaction_profile == "XR_NULL_PATH") {
            ImGui::Text("Interaction profile not loaded, try putting on your headset.");
            return;
        }

        if (ImGui::TreeNode(name.data())) {
            auto& hand = this->hands[index];

            std::vector<std::string> known_actions{};
            std::vector<const char*> known_actions_cstr{};

            for (auto action :this->action_set.actions) {
                known_actions.push_back(this->action_set.action_names[action]);
            }

            for (auto& it : known_actions) {
                known_actions_cstr.push_back(it.data());
            };

            auto& path_map = hand.path_map[current_interaction_profile];
            
            for (auto& it : path_map) {
                int current_combo_index = 0;

                for (auto i = 0; i < known_actions.size(); i++) {
                    if (known_actions[i] == it.first) {
                        current_combo_index = i;
                        break;
                    }
                }
                
                auto combo_name = this->get_path_string(it.second) + ": " + known_actions[current_combo_index];

                if (ImGui::Combo(combo_name.c_str(), &current_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                    path_map.erase(it.first);
                    path_map[known_actions[current_combo_index]] = it.second;

                    nlohmann::json j;

                    for (auto& it : this->hands[VRRuntime::Hand::LEFT].path_map[current_interaction_profile]) {
                        nlohmann::json binding{};
                        binding["action"] = it.first;
                        binding["path"] = this->get_path_string(it.second);
                        j["bindings"].push_back(binding);
                    }

                    for (auto& it : this->hands[VRRuntime::Hand::RIGHT].path_map[current_interaction_profile]) {
                        nlohmann::json binding{};
                        binding["action"] = it.first;
                        binding["path"] = this->get_path_string(it.second);
                        j["bindings"].push_back(binding);
                    }
                    
                    auto filename = this->get_current_interaction_profile() + ".json";
                    
                    // replace the slashes with underscores
                    std::replace(filename.begin(), filename.end(), '/', '_');
                    std::ofstream(filename) << j.dump(4);

                    this->wants_reinitialize = true;
                    break;
                }
            }

            ImGui::TreePop();
        }
    };

    display_hand("Left", 0);
    display_hand("Right", 1);
}

XrResult OpenXR::begin_frame() {
    std::scoped_lock _{sync_mtx};

    if (!this->ready() || !this->got_first_poses || !this->frame_synced) {
        //spdlog::info("VR: begin_frame: not ready");
        return XR_ERROR_SESSION_NOT_READY;
    }

    if (this->frame_began) {
        spdlog::info("[VR] begin_frame called while frame already began");
        return XR_SUCCESS;
    }

    this->begin_profile();

    XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    auto result = xrBeginFrame(this->session, &frame_begin_info);

    this->end_profile("xrBeginFrame");

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrBeginFrame failed: {}", this->get_result_string(result));
    }

    if (result == XR_ERROR_CALL_ORDER_INVALID) {
        synchronize_frame();
        result = xrBeginFrame(this->session, &frame_begin_info);
    }

    this->frame_began = result == XR_SUCCESS || result == XR_FRAME_DISCARDED; // discarded means endFrame was not called

    return result;
}

XrResult OpenXR::end_frame() {
    std::scoped_lock _{sync_mtx};

    if (!this->ready() || !this->got_first_poses || !this->frame_synced) {
        return XR_ERROR_SESSION_NOT_READY;
    }

    if (!this->frame_began) {
        spdlog::info("[VR] end_frame called while frame not begun");
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    std::vector<XrCompositionLayerBaseHeader*> layers{};
    std::vector<XrCompositionLayerProjectionView> projection_layer_views{};

    // we CANT push the layers every time, it cause some layer error
    // in xrEndFrame, so we must only do it when shouldRender is true
    if (this->frame_state.shouldRender == XR_TRUE) {
        projection_layer_views.resize(this->stage_views.size(), {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        for (auto i = 0; i < projection_layer_views.size(); ++i) {
            const auto& swapchain = this->swapchains[i];

            projection_layer_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projection_layer_views[i].pose = this->stage_views[i].pose;
            projection_layer_views[i].fov = this->stage_views[i].fov;
            projection_layer_views[i].subImage.swapchain = swapchain.handle;
            projection_layer_views[i].subImage.imageRect.offset = {0, 0};
            projection_layer_views[i].subImage.imageRect.extent = {swapchain.width, swapchain.height};
        }

        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = this->stage_space;
        layer.viewCount = (uint32_t)projection_layer_views.size();
        layer.views = projection_layer_views.data();
        layers.push_back((XrCompositionLayerBaseHeader*)&layer);
    }

    XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
    frame_end_info.displayTime = this->frame_state.predictedDisplayTime;
    frame_end_info.environmentBlendMode = this->blend_mode;
    frame_end_info.layerCount = (uint32_t)layers.size();
    frame_end_info.layers = layers.data();

    //spdlog::info("[VR] Ending frame, {} layers", frame_end_info.layerCount);
    //spdlog::info("[VR] Ending frame, layer ptr: {:x}", (uintptr_t)frame_end_info.layers);

    this->begin_profile();
    auto result = xrEndFrame(this->session, &frame_end_info);
    this->end_profile("xrEndFrame");
    
    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrEndFrame failed: {}", this->get_result_string(result));
    }
    
    this->frame_began = false;
    this->frame_synced = false;

    return result;
}
}
