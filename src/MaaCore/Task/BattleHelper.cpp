#include "BattleHelper.h"

#include <future>
#include <thread>

#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Config/TaskData.h"
#include "Controller.h"
#include "Task/ProcessTask.h"
#include "Utils/ImageIO.hpp"
#include "Utils/Logger.hpp"
#include "Utils/NoWarningCV.h"
#include "Vision/MatchImageAnalyzer.h"
#include "Vision/Miscellaneous/BattleImageAnalyzer.h"
#include "Vision/Miscellaneous/BattleSkillReadyImageAnalyzer.h"
#include "Vision/OcrWithPreprocessImageAnalyzer.h"

using namespace asst::battle;

bool asst::BattleHelper::set_stage_name(const std::string& name)
{
    LogTraceFunction;

    if (!Tile.contains(name)) {
        return false;
    }
    m_stage_name = name;
    return true;
}

bool asst::BattleHelper::calc_tiles_info(const std::string& stage_name)
{
    LogTraceFunction;

    auto normal_temp = Tile.calc(stage_name, false);
    if (normal_temp.empty()) {
        return false;
    }

    auto side_temp = Tile.calc(stage_name, true);
    if (side_temp.empty()) {
        return false;
    }

    m_normal_tile_info = std::move(normal_temp);
    m_side_tile_info = std::move(side_temp);

    return true;
}

bool asst::BattleHelper::pause()
{
    LogTraceFunction;

    return ProcessTask(*dynamic_cast<AbstractTask*>(this), { "BattlePause" }).run();
}

bool asst::BattleHelper::speed_up()
{
    LogTraceFunction;

    return ProcessTask(*dynamic_cast<AbstractTask*>(this), { "BattleSpeedUp" }).run();
}

bool asst::BattleHelper::analyze_deployment_opers(bool init)
{
    LogTraceFunction;

    if (init) {
        wait_for_start();
    }

    cv::Mat image = ctrler()->get_image();

    if (init) {
        auto draw_future = std::async(std::launch::async, [&]() { save_map(image); });

        // 识别一帧总击杀数
        BattleImageAnalyzer kills_analyzer(image);
        kills_analyzer.set_target(BattleImageAnalyzer::Target::Kills);
        if (kills_analyzer.analyze()) {
            m_kills = kills_analyzer.get_kills();
            m_total_kills = kills_analyzer.get_total_kills();
        }
    }

    BattleImageAnalyzer oper_analyzer(image);
    oper_analyzer.set_target(BattleImageAnalyzer::Target::Oper);
    if (!oper_analyzer.analyze()) {
        return false;
    }

    MatchImageAnalyzer avatar_analyzer(image);
    avatar_analyzer.set_task_info("BattleAvatarData");

    auto cur_opers = oper_analyzer.get_opers();
    std::vector<DeploymentOper> unknown_opers;

    for (auto& oper : cur_opers) {
        if (oper.cooling) {
            continue;
        }
        // 把当前ROI放大一点，不然有时候像素歪了容易匹配不上
        avatar_analyzer.set_roi(oper.rect.center_zoom(1.1, WindowWidthDefault, WindowHeightDefault));

        double max_socre = 0;

        for (const auto& [name, avatars] : m_all_deployment_avatars) {
            avatar_analyzer.set_templ(avatars);
            if (!avatar_analyzer.analyze()) {
                continue;
            }
            const auto& cur_matched = avatar_analyzer.get_result();
            if (max_socre < cur_matched.score) {
                max_socre = cur_matched.score;
                oper.name = name;
            }
        }

        if (max_socre) {
            m_cur_deployment_opers.insert_or_assign(oper.name, oper);
        }
        else {
            unknown_opers.emplace_back(oper);
        }
    }

    if (!unknown_opers.empty()) {
        // 一个都没匹配上的，挨个点开来看一下
        LogTraceScope("rec unknown opers");

        // 暂停游戏准备识别干员
        do {
            pause();
            // 在刚进入游戏的时候（画面刚刚完全亮起来的时候），点暂停是没反应的
            // 所以这里一直点，直到真的点上了为止
            if (!init || !check_pause_button()) {
                break;
            }
            std::this_thread::yield();
        } while (!need_exit());

        for (auto& oper : unknown_opers) {
            click_oper_on_deployment(oper.rect);

            OcrWithPreprocessImageAnalyzer name_analyzer(ctrler()->get_image());
            name_analyzer.set_task_info("BattleOperName");
            name_analyzer.set_replace(Task.get<OcrTaskInfo>("CharsNameOcrReplace")->replace_map);
            if (name_analyzer.analyze()) {
                Log.error("ocr failed");
                continue;
            }
            name_analyzer.sort_result_by_score();
            const std::string& name = name_analyzer.get_result().front().text;
            oper.name = name;
            m_cur_deployment_opers.insert_or_assign(name, oper);
            m_all_deployment_avatars.insert_or_assign(name, oper);
        }
        pause();
    }

    return true;
}

bool asst::BattleHelper::deploy_oper(const std::string& name, const Point& loc, DeployDirection direction)
{
    LogTraceFunction;

    const auto swipe_oper_task_ptr = Task.get("BattleSwipeOper");
    const auto use_oper_task_ptr = Task.get("BattleUseOper");

    auto rect_opt = get_oper_rect_on_deployment(name);
    if (!rect_opt) {
        return false;
    }
    const Rect& oper_rect = *rect_opt;

    auto target_iter = m_side_tile_info.find(loc);
    if (target_iter == m_side_tile_info.cend()) {
        Log.error("No loc", loc);
        return false;
    }
    const Point& target_point = target_iter->second.pos;

    int dist = static_cast<int>(
        Point::distance(target_point, { oper_rect.x + oper_rect.width / 2, oper_rect.y + oper_rect.height / 2 }));

    // 1000 是随便取的一个系数，把整数的 pre_delay 转成小数用的
    int duration = static_cast<int>(dist / 1000.0 * swipe_oper_task_ptr->pre_delay);
    bool deploy_with_pause = ctrler()->support_swipe_with_pause();
    ctrler()->swipe(oper_rect, Rect(target_point.x, target_point.y, 1, 1), duration, false,
                    swipe_oper_task_ptr->special_params.at(1), swipe_oper_task_ptr->special_params.at(2),
                    deploy_with_pause);

    // 拖动干员朝向
    if (direction != DeployDirection::None) {
        static const std::unordered_map<DeployDirection, Point> DirectionMap = {
            { DeployDirection::Right, Point(1, 0) }, { DeployDirection::Down, Point(0, 1) },
            { DeployDirection::Left, Point(-1, 0) }, { DeployDirection::Up, Point(0, -1) },
            { DeployDirection::None, Point(0, 0) },
        };

        // 计算往哪边拖动
        const Point& direction_target = DirectionMap.at(direction);

        // 将方向转换为实际的 swipe end 坐标点
        static const int coeff = swipe_oper_task_ptr->special_params.at(0);
        Point end_point = target_point + (direction_target * coeff);

        ctrler()->swipe(target_point, end_point, swipe_oper_task_ptr->post_delay);
        sleep(use_oper_task_ptr->post_delay);
    }

    if (deploy_with_pause) {
        ctrler()->press_esc();
    }

    BattlefieldOper bf_oper { .name = name, .loc = loc };
    m_battlefield_opers.emplace(name, bf_oper);
    m_used_tiles.emplace(loc, bf_oper);

    return true;
}

bool asst::BattleHelper::retreat_oper(const std::string& name)
{
    LogTraceFunction;

    auto oper_iter = m_battlefield_opers.find(name);
    if (oper_iter == m_battlefield_opers.cend()) {
        Log.error("No oper", name);
        return false;
    }

    if (!retreat_oper(oper_iter->second.loc, false)) {
        return false;
    }

    m_battlefield_opers.erase(name);
    return true;
}

bool asst::BattleHelper::retreat_oper(const Point& loc, bool manually)
{
    LogTraceFunction;

    if (!click_oper_on_battlefiled(loc) || !click_retreat()) {
        return false;
    }

    m_used_tiles.erase(loc);
    if (manually) {
        std::erase_if(m_battlefield_opers, [&loc](const auto& pair) -> bool { return pair.second.loc == loc; });
    }
    return true;
}

bool asst::BattleHelper::use_skill(const std::string& name)
{
    LogTraceFunction;

    auto oper_iter = m_battlefield_opers.find(name);
    if (oper_iter == m_battlefield_opers.cend()) {
        Log.error("No oper", name);
        return false;
    }

    return use_skill(oper_iter->second.loc);
}

bool asst::BattleHelper::use_skill(const Point& loc)
{
    LogTraceFunction;

    return click_oper_on_battlefiled(loc) && click_skill();
}

bool asst::BattleHelper::check_pause_button()
{
    MatchImageAnalyzer battle_flag_analyzer;
    battle_flag_analyzer.set_task_info("BattleOfficiallyBegin");
    battle_flag_analyzer.set_image(ctrler()->get_image());
    return battle_flag_analyzer.analyze();
}

bool asst::BattleHelper::wait_for_start()
{
    LogTraceFunction;

    while (!need_exit() && !check_pause_button()) {
        std::this_thread::yield();
    }
    return true;
}

bool asst::BattleHelper::wait_for_end()
{
    LogTraceFunction;

    while (!need_exit() && check_pause_button()) {
        use_all_ready_skill();
        std::this_thread::yield();
    }
    return true;
}

bool asst::BattleHelper::use_all_ready_skill()
{
    for (const auto& [name, loc] : m_battlefield_opers | std::views::values) {
        auto& usage = m_skill_usage[name];
        if (usage != SkillUsage::Possibly && usage != SkillUsage::Once) {
            continue;
        }
        if (!check_and_use_skill(loc)) {
            continue;
        }
        if (usage == SkillUsage::Once) {
            usage = SkillUsage::OnceUsed;
        }
    }
}

bool asst::BattleHelper::check_and_use_skill(const std::string& name)
{
    LogTraceFunction;

    auto oper_iter = m_battlefield_opers.find(name);
    if (oper_iter == m_battlefield_opers.cend()) {
        Log.error("No oper", name);
        return false;
    }
    return check_and_use_skill(oper_iter->second.loc);
}

bool asst::BattleHelper::check_and_use_skill(const Point& loc)
{
    LogTraceFunction;

    BattleSkillReadyImageAnalyzer skill_analyzer(ctrler()->get_image());

    auto target_iter = m_normal_tile_info.find(loc);
    if (target_iter == m_normal_tile_info.end()) {
        Log.error("No loc", loc);
        return false;
    }
    const Point& battlefield_point = target_iter->second.pos;
    skill_analyzer.set_base_point(battlefield_point);
    if (!skill_analyzer.analyze()) {
        return false;
    }

    return use_skill(loc);
}

void asst::BattleHelper::save_map(const cv::Mat& image)
{
    LogTraceFunction;

    using namespace asst::utils::path_literals;
    const auto& MapDir = "map"_p;

    std::filesystem::create_directories(MapDir);
    auto draw = image.clone();

    for (const auto& [loc, info] : m_normal_tile_info) {
        std::string text = "( " + std::to_string(loc.x) + ", " + std::to_string(loc.y) + " )";
        cv::putText(draw, text, cv::Point(info.pos.x - 30, info.pos.y), 1, 1.2, cv::Scalar(0, 0, 255), 2);
    }
    asst::imwrite(MapDir / asst::utils::path(m_stage_name + ".png"), draw);
}

bool asst::BattleHelper::click_oper_on_deployment(const std::string& name)
{
    LogTraceFunction;

    auto rect_opt = get_oper_rect_on_deployment(name);
    if (!rect_opt) {
        return false;
    }

    return click_oper_on_deployment(*rect_opt);
}

bool asst::BattleHelper::click_oper_on_deployment(const Rect& rect)
{
    LogTraceFunction;

    const auto use_oper_task_ptr = Task.get("BattleUseOper");
    ctrler()->click(rect);
    sleep(use_oper_task_ptr->pre_delay);

    return true;
}

bool asst::BattleHelper::click_oper_on_battlefiled(const std::string& name)
{
    LogTraceFunction;

    auto oper_iter = m_battlefield_opers.find(name);
    if (oper_iter == m_battlefield_opers.cend()) {
        Log.error("No oper", name);
        return false;
    }
    return click_oper_on_battlefiled(oper_iter->second.loc);
}

bool asst::BattleHelper::click_oper_on_battlefiled(const Point& loc)
{
    LogTraceFunction;

    const auto use_oper_task_ptr = Task.get("BattleUseOper");

    auto target_iter = m_normal_tile_info.find(loc);
    if (target_iter == m_normal_tile_info.end()) {
        Log.error("No loc", loc);
        return false;
    }
    const Point& target_point = target_iter->second.pos;

    ctrler()->click(target_point);
    sleep(use_oper_task_ptr->pre_delay);

    return true;
}

bool asst::BattleHelper::click_retreat()
{
    LogTraceFunction;

    return ProcessTask(*dynamic_cast<AbstractTask*>(this), { "BattleOperRetreatJustClick" }).run();
}

bool asst::BattleHelper::click_skill()
{
    LogTraceFunction;

    return ProcessTask(*dynamic_cast<AbstractTask*>(this), { "BattleSkillReadyOnClick", "BattleSkillStopOnClick" })
        .set_task_delay(0)
        .set_retry_times(1000)
        .run();
}

std::optional<asst::Rect> asst::BattleHelper::get_oper_rect_on_deployment(const std::string& name) const
{
    LogTraceFunction;

    auto oper_iter = m_cur_deployment_opers.find(name);
    if (oper_iter == m_cur_deployment_opers.cend()) {
        Log.error("No oper", name);
        return std::nullopt;
    }

    return oper_iter->second.rect;
}
