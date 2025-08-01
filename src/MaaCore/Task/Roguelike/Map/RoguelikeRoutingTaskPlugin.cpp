#include "RoguelikeRoutingTaskPlugin.h"

#include <limits>
#include <numeric>

#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/ProcessTask.h"
#include "Utils/ImageIo.hpp"
#include "Utils/Logger.hpp"
#include "Utils/NoWarningCV.h"
#include "Vision/Matcher.h"
#include "Vision/Miscellaneous/BrightPointAnalyzer.h"
#include "Vision/MultiMatcher.h"

bool asst::RoguelikeRoutingTaskPlugin::load_params([[maybe_unused]] const json::value& params)
{
    const std::string& theme = m_config->get_theme();

    // 本插件暂处于实验阶段，仅用于萨卡兹和界园肉鸽的第一层
    if (theme != RoguelikeTheme::Sarkaz && theme != RoguelikeTheme::JieGarden) {
        return false;
    }

    const std::shared_ptr<MatchTaskInfo> config = Task.get<MatchTaskInfo>(theme + "@Roguelike@RoutingConfig");

    m_origin_x = config->special_params.at(0);
    m_middle_x = config->special_params.at(1);
    m_last_x = config->special_params.at(2);
    m_node_width = config->special_params.at(3);
    m_node_height = config->special_params.at(4);
    m_column_offset = config->special_params.at(5);
    m_nameplate_offset = config->special_params.at(6);
    m_roi_margin = config->special_params.at(7);
    m_direction_threshold = config->special_params.at(8);

    const RoguelikeMode& mode = m_config->get_mode();
    const std::string squad = params.get("squad", "");

    if (theme == RoguelikeTheme::Sarkaz && mode == RoguelikeMode::Investment && squad == "点刺成锭分队") {
        m_routing_strategy = RoutingStrategy::FastInvestment_Sarkaz;
        return true;
    }

    if (theme == RoguelikeTheme::JieGarden && mode == RoguelikeMode::Investment && squad == "指挥分队" &&
        m_config->get_difficulty() >= 3) {
        m_routing_strategy = RoutingStrategy::FastInvestment_JieGarden;
        return true;
    }

    if (mode == RoguelikeMode::FastPass && squad == "蓝图测绘分队") {
        m_routing_strategy = RoutingStrategy::FastPass;
        return true;
    }

    return false;
}

void asst::RoguelikeRoutingTaskPlugin::reset_in_run_variables()
{
    m_map.reset();
    m_need_generate_map = true;
    m_selected_column = 0;
    m_selected_x = 0;
}

bool asst::RoguelikeRoutingTaskPlugin::verify(const AsstMsg msg, const json::value& details) const
{
    if (msg != AsstMsg::SubTaskStart || details.get("subtask", std::string()) != "ProcessTask") {
        return false;
    }

    std::string task_name = details.get("details", "task", "");

    // trigger 任务的名字可以为 "...@Roguelike@Routing-..." 的形式
    if (const size_t pos = task_name.find('-'); pos != std::string::npos) {
        task_name = task_name.substr(0, pos);
    }

    if (task_name == m_config->get_theme() + "@Roguelike@Routing") {
        return true;
    }

    return false;
}

bool asst::RoguelikeRoutingTaskPlugin::_run()
{
    LogTraceFunction;

    switch (m_routing_strategy) {
    case RoutingStrategy::FastInvestment_Sarkaz:
        if (m_need_generate_map) {
            // 随机点击一个第一列的节点，先随便写写，垃圾代码迟早要重构
            ProcessTask(*this, { "Sarkaz@Roguelike@Routing-CombatOps" }).run();
            // 刷新节点
            ProcessTask(*this, { m_config->get_theme() + "@Roguelike@RoutingRefreshNode" }).run();
            // 不识别了，进商店，Go!
            Task.set_task_base("Sarkaz@Roguelike@RoutingAction", "Sarkaz@Roguelike@RoutingAction-StageTraderEnter");
            // 偷懒，直接用 m_need_generate_map 判断是否已进过商店
            m_need_generate_map = false;
        }
        else {
            Task.set_task_base("Sarkaz@Roguelike@RoutingAction", "Sarkaz@Roguelike@RoutingAction-ExitThenAbandon");
        }
        break;
    case RoutingStrategy::FastInvestment_JieGarden:
        if (m_need_generate_map) {
            cv::Mat image = ctrler()->get_image();
            cv::Mat image_draw = image.clone();
            update_map(image, RoguelikeMap::INIT_INDEX + 1, image_draw);
#ifdef ASST_DEBUG
            const std::filesystem::path& relative_dir = utils::path("debug") / utils::path("roguelikeMap");
            const auto relative_path = relative_dir / (utils::get_time_filestem() + "_draw.png");
            Log.trace("Save image", relative_path);
            asst::imwrite(relative_path, image_draw);
#endif
            m_need_generate_map = false;
        }
        if (m_map.get_curr_pos() == RoguelikeMap::INIT_INDEX) {
            m_map.set_cost_fun([&](const RoguelikeNodePtr& node) {
                if (node->type == RoguelikeNodeType::CombatOps || node->type == RoguelikeNodeType::EmergencyOps ||
                    node->type == RoguelikeNodeType::DreadfulFoe) {
                    return 1;
                }
                return 0;
            });
            m_map.update_node_costs();
            const size_t next_node = m_map.get_next_node();

            // 若无法避免超过两场战斗则重开
            if (m_map.get_node_cost(next_node) >= 2) {
                callback(
                    AsstMsg::TaskChainExtraInfo,
                    json::object {
                        { "what", "RoutingRestart" },
                        { "why", "TooManyBattlesAhead" },
                        { "node_cost", m_map.get_node_cost(next_node) },
                    });

                Task.set_task_base(
                    "JieGarden@Roguelike@RoutingAction",
                    "JieGarden@Roguelike@RoutingAction-ExitThenAbandon");
                reset_in_run_variables();
            }
            else {
                const int next_node_x = m_origin_x;
                const int next_node_y = m_map.get_node_y(next_node);
                Point next_node_center = Point(next_node_x + m_node_width / 2, next_node_y + m_node_height / 2);
                ctrler()->click(next_node_center);
                sleep(200);

                Task.set_task_base(
                    "JieGarden@Roguelike@RoutingAction",
                    "JieGarden@Roguelike@RoutingAction-StageCombatOpsEnterThenLeave");
                m_map.set_curr_pos(next_node);
            }
        }
        else {
            // 执行默认的避战策略
            Task.set_task_base("JieGarden@Roguelike@RoutingAction", "JieGarden@Roguelike@Stages_default");
        }
        break;
    case RoutingStrategy::FastPass:
        if (m_need_generate_map) {
            generate_map();
            m_need_generate_map = false;
        }

        m_selected_column = m_map.get_node_column(m_map.get_curr_pos());
        update_selected_x();

        refresh_following_combat_nodes();
        navigate_route();
        break;

    default:
        break;
    }

    return true;
}

bool asst::RoguelikeRoutingTaskPlugin::update_map(
    const cv::Mat& image,
    const size_t leftmost_column,
    std::optional<std::reference_wrapper<cv::Mat>> image_draw_opt)
{
    LogTraceFunction;

    if (leftmost_column == 0) {
        Log.error(__FUNCTION__, "| leftmost_column must be greater than zero");
        return false;
    }

    const std::string& theme = m_config->get_theme();

    size_t curr_col = leftmost_column - 1;
    int curr_x = -m_node_width - 1; // 第一列节点将触发 rect.x >= curr_x + m_node_width 并更新 curr_col 与 curr_x

    MultiMatcher node_analyzer(image);
    node_analyzer.set_task_info(theme + "@Roguelike@RoutingNodeAnalyze");
    if (!node_analyzer.analyze()) {
        Log.error(__FUNCTION__, "| no nodes are recognised");
        return false;
    }
    MultiMatcher::ResultsVec match_results = node_analyzer.get_result();
    sort_by_vertical_(match_results); // 按照水平方向从左到右排序各列节点，同一列节点按照垂直方向从上到下排序

    const size_t old_num_columns = m_map.get_num_columns();
    for (const auto& [rect, score, templ_name] : match_results) {
        const RoguelikeNodeType type = RoguelikeMapInfo.templ2type(theme, templ_name);
#ifdef ASST_DEBUG
        if (image_draw_opt.has_value()) {
            cv::rectangle(image_draw_opt.value().get(), make_rect<cv::Rect>(rect), cv::Scalar(255, 255, 255), 2);
            cv::putText(
                image_draw_opt.value().get(),
                templ_name,
                cv::Point(rect.x, rect.y - m_roi_margin),
                cv::FONT_HERSHEY_DUPLEX,
                0.5,
                cv::Scalar(255, 255, 255));
        }
#endif
        if (rect.x >= curr_x + m_node_width) { // 识别到下一列的节点
            ++curr_col;
            curr_x = rect.x;
        }
        if (curr_col >= old_num_columns) // 仅更新新列节点
        {
            const size_t node = m_map.create_and_insert_node(type, curr_col, rect.y).value();
            generate_edges(node, image, rect.x, image_draw_opt);
        }
    }

    return true;
}

void asst::RoguelikeRoutingTaskPlugin::generate_map()
{
    LogTraceFunction;

    const std::string& theme = m_config->get_theme();

    m_map.reset();
    size_t curr_col = RoguelikeMap::INIT_INDEX + 1;
    Rect roi = Task.get<MatchTaskInfo>(theme + "@Roguelike@RoutingNodeAnalyze")->roi;

    // 第一列节点
    cv::Mat image = ctrler()->get_image();
    MultiMatcher node_analyzer(image);
    node_analyzer.set_task_info(theme + "@Roguelike@RoutingNodeAnalyze");
    if (!node_analyzer.analyze()) {
        Log.error(__FUNCTION__, "| no nodes found in the first column");
        return;
    }
    MultiMatcher::ResultsVec match_results = node_analyzer.get_result();
    sort_by_vertical_(match_results); // 按照垂直方向排序（从上到下）
    for (const auto& [rect, score, templ_name] : match_results) {
        const RoguelikeNodeType type = RoguelikeMapInfo.templ2type(theme, templ_name);
        const size_t node = m_map.create_and_insert_node(type, curr_col, rect.y).value();
        generate_edges(node, image, rect.x);
    }

    // 第二列及以后的节点
    roi.x += m_column_offset;
    node_analyzer.set_roi(roi);
    while (!need_exit() && node_analyzer.analyze()) {
        ++curr_col;
        match_results = node_analyzer.get_result();
        sort_by_vertical_(match_results);
        for (const auto& [rect, score, templ_name] : match_results) {
            const RoguelikeNodeType type = RoguelikeMapInfo.templ2type(theme, templ_name);
            const size_t node = m_map.create_and_insert_node(type, curr_col, rect.y).value();
            generate_edges(node, image, rect.x);
        }
        ProcessTask(*this, { theme + "@Roguelike@RoutingSwipeRight" }).run();
        sleep(200);
        image = ctrler()->get_image();
        node_analyzer.set_image(image);
    }

    ProcessTask(*this, { theme + "@Roguelike@RoutingExitThenContinue" }).run(); // 通过退出重进回到初始位置
}

void asst::RoguelikeRoutingTaskPlugin::generate_edges(
    const size_t& node,
    const cv::Mat& image,
    const int& node_x,
    [[maybe_unused]] std::optional<std::reference_wrapper<cv::Mat>> image_draw_opt)
{
    LogTraceFunction;

    const size_t node_column = m_map.get_node_column(node);

    if (node_column == RoguelikeMap::INIT_INDEX) {
        Log.error(__FUNCTION__, "| cannot generate edges for init node");
        return;
    }

    if (node_column == RoguelikeMap::INIT_INDEX + 1) {
        m_map.add_edge(RoguelikeMap::INIT_INDEX, node); // 第一列节点直接与 init 连接
        return;
    }

    // 将 image转换为二值图像后计算亮点
    BrightPointAnalyzer analyzer(image);

    const int center_x = node_x - (m_column_offset - m_node_width) / 2; // node 与 前一列节点的中点横坐标
    const int node_y = m_map.get_node_y(node);
    Rect roi(0, 0, m_roi_margin * 2, m_roi_margin * 2);

    // 遍历前一列节点
    const size_t pre_col_begin = m_map.get_column_begin(node_column - 1);
    const size_t pre_col_end = m_map.get_column_end(node_column - 1);
    for (size_t prev = pre_col_begin; prev < pre_col_end; ++prev) {
        const int prev_y = m_map.get_node_y(prev);
        const int center_y = (prev_y + node_y + m_node_height) / 2;
        roi.x = center_x - m_roi_margin;
        roi.y = center_y - m_roi_margin;
#ifdef ASST_DEBUG
        if (image_draw_opt.has_value()) {
            cv::rectangle(image_draw_opt.value().get(), make_rect<cv::Rect>(roi), cv::Scalar(255, 255, 255), 1);
        }
#endif
        analyzer.set_roi(roi);

        if (!analyzer.analyze()) { // 节点间没有连线
            continue;
        }

        // 按照水平方向排序（从左到右）
        std::vector<Point> brightPoints = analyzer.get_result();

        auto [x_min_p, x_max_p] = ranges::minmax(brightPoints, /*comp=*/ {}, [](const Point& p) { return p.x; });
        const int leftmost_x = x_min_p.x;
        const int rightmost_x = x_max_p.x;

        auto leftmostBrightPoints = brightPoints | views::filter([&](const Point& p) { return p.x == leftmost_x; });
        auto rightmostBrightPoints = brightPoints | views::filter([&](const Point& p) { return p.x == rightmost_x; });

        auto [leftmost_y_min_p, leftmost_y_max_p] =
            ranges::minmax(leftmostBrightPoints, /*comp=*/ {}, [](const Point& p) { return p.y; });
        const int leftmost_y = (leftmost_y_min_p.y + leftmost_y_max_p.y) / 2;

        auto [rightmost_y_min_p, rightmost_y_max_p] =
            ranges::minmax(rightmostBrightPoints, /*comp=*/ {}, [](const Point& p) { return p.y; });
        const int rightmost_y = (rightmost_y_min_p.y + rightmost_y_max_p.y) / 2;

        if ((std::abs(prev_y - node_y) < m_direction_threshold &&
             std::abs(leftmost_y - rightmost_y) < m_direction_threshold) ||
            (prev_y < node_y && leftmost_y < rightmost_y - m_direction_threshold) ||
            (prev_y > node_y && leftmost_y > rightmost_y + m_direction_threshold)) {
            m_map.add_edge(prev, node);
#ifdef ASST_DEBUG
            if (image_draw_opt.has_value()) {
                cv::line(
                    image_draw_opt.value().get(),
                    cv::Point(node_x - m_column_offset + m_node_width / 2, prev_y + m_node_height / 2),
                    cv::Point(node_x + m_node_width / 2, node_y + m_node_height / 2),
                    cv::Scalar(255, 255, 255),
                    2);
            }
#endif
        }
    }

    // 同列前一个节点
    if (node > m_map.get_column_begin(node_column)) {
        size_t prev = node - 1;
        roi.x = node_x + m_node_width / 2 - m_roi_margin;
        roi.y = (m_map.get_node_y(prev) + m_node_height + m_nameplate_offset + node_y) / 2 - m_roi_margin;
#ifdef ASST_DEBUG
        if (image_draw_opt.has_value()) {
            cv::rectangle(image_draw_opt.value().get(), make_rect<cv::Rect>(roi), cv::Scalar(255, 255, 255), 1);
        }
#endif
        analyzer.set_roi(roi);
        if (analyzer.analyze()) {
            m_map.add_edge(prev, node);
            m_map.add_edge(node, prev);
#ifdef ASST_DEBUG
            if (image_draw_opt.has_value()) {
                cv::line(
                    image_draw_opt.value().get(),
                    cv::Point(node_x + m_node_width / 2, m_map.get_node_y(prev) + m_node_height / 2),
                    cv::Point(node_x + m_node_width / 2, node_y + m_node_height / 2),
                    cv::Scalar(255, 255, 255),
                    2);
            }
#endif
        }
    }
}

void asst::RoguelikeRoutingTaskPlugin::refresh_following_combat_nodes()
{
    LogTraceFunction;

    const std::string& theme = m_config->get_theme();

    const size_t curr_node = m_map.get_curr_pos();
    const size_t curr_node_column = m_map.get_node_column(curr_node);

    for (size_t next_node : m_map.get_node_succs(curr_node)) {
        // 不刷新同一列的节点
        const size_t next_node_column = m_map.get_node_column(next_node);
        if (next_node_column <= curr_node_column) {
            continue;
        }
        // 每个节点仅刷新一次
        if (m_map.get_node_refresh_times(next_node)) {
            continue;
        }
        // 不刷新非战斗节点
        RoguelikeNodeType next_node_type = m_map.get_node_type(next_node);
        if (next_node_type != RoguelikeNodeType::CombatOps && next_node_type != RoguelikeNodeType::EmergencyOps &&
            next_node_type != RoguelikeNodeType::DreadfulFoe) {
            continue;
        }

        int next_node_x = m_selected_x + (next_node_column == m_selected_column ? 0 : m_column_offset);
        int next_node_y = m_map.get_node_y(next_node);
        Rect next_node_rect = Rect(next_node_x, next_node_y, m_node_width, m_node_height);

        // 点击节点
        ctrler()->click(next_node_rect);
        m_selected_column = m_map.get_node_column(next_node);
        update_selected_x();
        next_node_rect.x = m_selected_x;
        sleep(200);

        // 刷新节点
        ProcessTask(*this, { m_config->get_theme() + "@Roguelike@RoutingRefreshNode" }).run();
        m_map.set_node_refresh_times(next_node, m_map.get_node_refresh_times(next_node) + 1);

        // 识别并更新节点类型
        Matcher node_analyzer(ctrler()->get_image());
        node_analyzer.set_task_info(theme + "@Roguelike@RoutingNodeAnalyze");
        node_analyzer.set_roi(next_node_rect);
        if (node_analyzer.analyze()) {
            const Matcher::Result& match_results = node_analyzer.get_result();
            m_map.set_node_type(next_node, RoguelikeMapInfo.templ2type(theme, match_results.templ_name));
        }
    }
}

void asst::RoguelikeRoutingTaskPlugin::navigate_route()
{
    LogTraceFunction;

    const size_t curr_col = m_map.get_node_column(m_map.get_curr_pos());

    m_map.set_cost_fun([&](const RoguelikeNodePtr& node) {
        if (node->visited) {
            return 1000;
        }

        if (node->column == curr_col) {
            return 1000;
        }

        if (node->type == RoguelikeNodeType::CombatOps || node->type == RoguelikeNodeType::EmergencyOps ||
            node->type == RoguelikeNodeType::DreadfulFoe) {
            return 1 + (node->refresh_times ? 999 : 0);
        }

        return 0;
    });

    m_map.update_node_costs();

    const size_t next_node = m_map.get_next_node();

    if (m_map.get_node_cost(next_node) >= 1000) {
        Task.set_task_base("Sarkaz@Roguelike@RoutingAction", "Sarkaz@Roguelike@RoutingAction-ExitThenAbandon");
        reset_in_run_variables();
        return;
    }

    const size_t next_node_column = m_map.get_node_column(next_node);
    const int next_node_x = m_selected_x + (next_node_column == m_selected_column ? 0 : m_column_offset);
    const int next_node_y = m_map.get_node_y(next_node);
    Point next_node_center = Point(next_node_x + m_node_width / 2, next_node_y + m_node_height / 2);
    ctrler()->click(next_node_center);
    sleep(200);

    if (m_map.get_node_type(next_node) == RoguelikeNodeType::Encounter) {
        Task.set_task_base("Sarkaz@Roguelike@RoutingAction", "Sarkaz@Roguelike@RoutingAction-StageEncounterEnter");
        m_map.set_curr_pos(next_node);
    }
    else if (m_map.get_node_type(next_node) == RoguelikeNodeType::RogueTrader) {
        Task.set_task_base("Sarkaz@Roguelike@RoutingAction", "Sarkaz@Roguelike@RoutingAction-StageTraderEnter");
        reset_in_run_variables();
    }
    else {
        Task.set_task_base("Sarkaz@Roguelike@RoutingAction", "Sarkaz@Roguelike@RoutingAction-ExitThenAbandon");
        reset_in_run_variables();
    }
}

void asst::RoguelikeRoutingTaskPlugin::update_selected_x()
{
    if (m_selected_column == RoguelikeMap::INIT_INDEX) {
        m_selected_x = m_origin_x - m_column_offset;
    }
    else if (m_selected_column == RoguelikeMap::INIT_INDEX + 1) {
        m_selected_x = m_origin_x;
    }
    else if (m_selected_column == m_map.get_num_columns() - 1) [[unlikely]] {
        m_selected_x = m_last_x;
    }
    else {
        m_selected_x = m_middle_x;
    }
}
