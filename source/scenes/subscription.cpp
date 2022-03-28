#include "headers.hpp"
#include <vector>
#include <string>
#include <set>
#include <map>
#include <numeric>

#include "scenes/subscription.hpp"
#include "system/util/subscription_util.hpp"
#include "scenes/video_player.hpp"
#include "youtube_parser/parser.hpp"
#include "ui/ui.hpp"
#include "ui/overlay.hpp"
#include "network/thumbnail_loader.hpp"
#include "system/util/misc_tasks.hpp"
#include "system/util/async_task.hpp"

#define MAX_THUMBNAIL_LOAD_REQUEST 12

#define FEED_RELOAD_BUTTON_HEIGHT 18
#define TOP_HEIGHT (MIDDLE_FONT_INTERVAL + SMALL_MARGIN * 2)
#define VIDEO_TITLE_MAX_WIDTH (320 - SMALL_MARGIN * 2 - VIDEO_LIST_THUMBNAIL_WIDTH)

namespace Subscription {
	bool thread_suspend = false;
	bool already_init = false;
	bool exiting = false;
	
	Mutex resource_lock;
	
	std::vector<SubscriptionChannel> subscribed_channels;
	bool clicked_is_video;
	std::string clicked_url;
	
	int feed_loading_progress = 0;
	int feed_loading_total = 0;
	
	int CONTENT_Y_HIGH = 240; // changes according to whether the video playing bar is drawn or not
	
	VerticalListView *main_view = NULL;
	TabView *main_tab_view = NULL;
	ScrollView *channels_tab_view = NULL;
	VerticalListView *channels_tab_list_view = NULL;
	VerticalListView *feed_tab_view = NULL;
	ScrollView *feed_videos_view = NULL;
	VerticalListView *feed_videos_list_view = NULL;
};
using namespace Subscription;

static void load_subscription_feed(void *);
static void update_subscribed_channels(const std::vector<SubscriptionChannel> &new_subscribed_channels);

void Subscription_init(void) {
	Util_log_save("subsc/init", "Initializing...");
	
	channels_tab_list_view = (new VerticalListView(0, 0, 320))->set_margin(SMALL_MARGIN)
		->enable_thumbnail_request_update(MAX_THUMBNAIL_LOAD_REQUEST, SceneType::SUBSCRIPTION);
	channels_tab_view = (new ScrollView(0, 0, 320, 0))->set_views({channels_tab_list_view}); // height : dummy(set properly by set_stretch_subview(true) on main_tab_view)
	feed_videos_list_view = (new VerticalListView(0, 0, 320))->set_margin(SMALL_MARGIN)
		->enable_thumbnail_request_update(MAX_THUMBNAIL_LOAD_REQUEST, SceneType::SUBSCRIPTION);
	feed_videos_view = (new ScrollView(0, 0, 320, 0))->set_views({feed_videos_list_view});
	feed_tab_view = (new VerticalListView(0, 0, 320))
		->set_views({
			(new TextView(0, 0, 320, FEED_RELOAD_BUTTON_HEIGHT))
				->set_text((std::function<std::string ()>) [] () {
					auto res = LOCALIZED(RELOAD);
					if (is_async_task_running(load_subscription_feed)) res += " (" + std::to_string(feed_loading_progress) + "/" + std::to_string(feed_loading_total) + ")";
					return res;
				})
				->set_text_offset(SMALL_MARGIN, -1)
				->set_on_view_released([] (View &) {
					if (!is_async_task_running(load_subscription_feed))
						queue_async_task(load_subscription_feed, NULL);
				})
				->set_get_background_color([] (const View &view) -> u32 {
					if (is_async_task_running(load_subscription_feed)) return LIGHT0_BACK_COLOR;
					return View::STANDARD_BACKGROUND(view);
				}),
			(new HorizontalRuleView(0, 0, 320, SMALL_MARGIN))->set_get_background_color([] (const View &) { return DEFAULT_BACK_COLOR; }),
			feed_videos_view
		})
		->set_draw_order({2, 1, 0});
	main_tab_view = (new TabView(0, 0, 320, CONTENT_Y_HIGH - TOP_HEIGHT))
		->set_views({channels_tab_view, feed_tab_view})
		->set_tab_texts({
			(std::function<std::string ()>) [] () { return LOCALIZED(SUBSCRIBED_CHANNELS); },
			(std::function<std::string ()>) [] () { return LOCALIZED(NEW_VIDEOS); }
		});
	main_view = (new VerticalListView(0, 0, 320))
		->set_views({
			(new TextView(0, 0, 320, MIDDLE_FONT_INTERVAL))
				->set_text((std::function<std::string ()>) [] () { return LOCALIZED(SUBSCRIPTION); })
				->set_font_size(MIDDLE_FONT_SIZE, MIDDLE_FONT_INTERVAL)
				->set_get_background_color([] (const View &) { return DEFAULT_BACK_COLOR; }),
			(new HorizontalRuleView(0, 0, 320, SMALL_MARGIN * 2))
				->set_get_background_color([] (const View &) { return DEFAULT_BACK_COLOR; }),
			main_tab_view
		})
		->set_draw_order({2, 1, 0});
	
	Subscription_resume("");
	already_init = true;
}
void Subscription_exit(void) {
	already_init = false;
	thread_suspend = false;
	exiting = true;
	
	resource_lock.lock();
	
	main_view->recursive_delete_subviews();
	delete main_view;
	main_view = NULL;
	main_tab_view = NULL;
	feed_tab_view = NULL;
	feed_videos_view = NULL;
	feed_videos_list_view = NULL;
	channels_tab_view = NULL;
	channels_tab_list_view = NULL;
	
	resource_lock.unlock();
	
	Util_log_save("subsc/exit", "Exited.");
}
void Subscription_suspend(void) {
	thread_suspend = true;
}
void Subscription_resume(std::string arg) {
	(void) arg;
	
	// main_tab_view->on_resume();
	overlay_menu_on_resume();
	thread_suspend = false;
	var_need_reflesh = true;
	
	update_subscribed_channels(get_subscribed_channels());
}


// async functions
static void load_subscription_feed(void *) {
	resource_lock.lock();
	auto channels = subscribed_channels;
	resource_lock.unlock();
	
	feed_loading_progress = 0;
	feed_loading_total = channels.size();
	std::map<std::pair<int, int>, std::vector<YouTubeVideoSuccinct> > loaded_videos;
	for (auto channel : channels) {
		Util_log_save("subsc", "load " + channel.name);
		add_cpu_limit(ADDITIONAL_CPU_LIMIT);
		auto result = youtube_parse_channel_page(channel.url);
		remove_cpu_limit(ADDITIONAL_CPU_LIMIT);
		
		// update the subscription metadata at the same time
		if (result.name != "") {
			SubscriptionChannel new_info;
			new_info.id = result.id;
			new_info.url = result.url;
			new_info.name = result.name;
			new_info.icon_url = result.icon_url;
			new_info.subscriber_count_str = result.subscriber_count_str;
			subscription_unsubscribe(result.id);
			subscription_subscribe(new_info);
		}
		
		misc_tasks_request(TASK_SAVE_SUBSCRIPTION);
		var_need_reflesh = true;
		
		for (auto video : result.videos) {
			std::string date_number_str;
			for (auto c : video.publish_date) if (isdigit(c)) date_number_str.push_back(c);
			
			// 1 : seconds
			char *end;
			int number = strtoll(date_number_str.c_str(), &end, 10);
			if (*end) {
				Util_log_save("subsc", "failed to parse the integer in date : " + video.publish_date);
				continue;
			}
			int unit = -1;
			std::vector<std::vector<std::string> > unit_list = {
				{"second", "秒"},
				{"minute", "分"},
				{"hour", "時間"},
				{"day", "日"},
				{"week", "週間"},
				{"month", "月"},
				{"year", "年"}
			};
			for (size_t i = 0; i < unit_list.size(); i++) {
				bool matched = false;
				for (auto pattern : unit_list[i]) if (video.publish_date.find(pattern) != std::string::npos) {
					matched = true;
					break;
				}
				if (matched) {
					unit = i;
					break;
				}
			}
			if (unit == -1) {
				Util_log_save("subsc", "failed to parse the unit of date : " + video.publish_date);
				continue;
			}
			if (std::pair<int, int>{unit, number} > std::pair<int, int>{5, 2}) continue; // more than 2 months old
			Util_log_save("subsc", "+ : " + video.title);
			loaded_videos[{unit, number}].push_back(video);
		}
		feed_loading_progress++;
	}
	std::vector<View *> new_feed_video_views;
	for (auto &i : loaded_videos) {
		for (auto video : i.second) {
			SuccinctVideoView *cur_view = (new SuccinctVideoView(0, 0, 320, VIDEO_LIST_THUMBNAIL_HEIGHT));
			
			cur_view->set_title_lines(truncate_str(video.title, VIDEO_TITLE_MAX_WIDTH, 2, 0.5, 0.5));
			cur_view->set_thumbnail_url(video.thumbnail_url);
			cur_view->set_auxiliary_lines({video.publish_date, video.views_str});
			cur_view->set_bottom_right_overlay(video.duration_text);
			cur_view->set_get_background_color(View::STANDARD_BACKGROUND);
			cur_view->set_on_view_released([video] (View &view) {
				clicked_url = video.url;
				clicked_is_video = true;
			});
			
			new_feed_video_views.push_back(cur_view);
		}
	}
	
	misc_tasks_request(TASK_SAVE_SUBSCRIPTION);
	
	resource_lock.lock();
	if (exiting) { // app shut down while loading
		resource_lock.unlock();
		return;
	}
	update_subscribed_channels(get_subscribed_channels());
	
	feed_videos_list_view->recursive_delete_subviews();
	feed_videos_list_view->views = new_feed_video_views;
	resource_lock.unlock();
}

static void update_subscribed_channels(const std::vector<SubscriptionChannel> &new_subscribed_channels) {
	subscribed_channels = new_subscribed_channels;
	
	channels_tab_list_view->recursive_delete_subviews();
	
	// prepare new views
	for (auto channel : new_subscribed_channels) {
		SuccinctChannelView *cur_view = (new SuccinctChannelView(0, 0, 320, CHANNEL_ICON_HEIGHT));
		cur_view->set_name(channel.name);
		cur_view->set_auxiliary_lines({channel.subscriber_count_str});
		cur_view->set_thumbnail_url(channel.icon_url);
		cur_view->set_get_background_color(View::STANDARD_BACKGROUND);
		cur_view->set_on_view_released([channel] (View &view) {
			clicked_url = channel.url;
			clicked_is_video = false;
		});
		channels_tab_list_view->views.push_back(cur_view);
	}
}




Intent Subscription_draw(void) {
	Intent intent;
	intent.next_scene = SceneType::NO_CHANGE;
	Hid_info key;
	Util_hid_query_key_state(&key);
	
	thumbnail_set_active_scene(SceneType::SUBSCRIPTION);
	
	bool video_playing_bar_show = video_is_playing();
	CONTENT_Y_HIGH = video_playing_bar_show ? 240 - VIDEO_PLAYING_BAR_HEIGHT : 240;
	main_tab_view->update_y_range(0, CONTENT_Y_HIGH - TOP_HEIGHT);
	
	
	if(var_need_reflesh || !var_eco_mode)
	{
		var_need_reflesh = false;
		Draw_frame_ready();
		Draw_screen_ready(0, DEFAULT_BACK_COLOR);

		if(Util_log_query_log_show_flag())
			Util_log_draw();

		Draw_top_ui();
		if (var_debug_mode) Draw_debug_info();
		
		Draw_screen_ready(1, DEFAULT_BACK_COLOR);
		
		resource_lock.lock();
		main_view->draw();
		resource_lock.unlock();
		
		if (video_playing_bar_show) video_draw_playing_bar();
		draw_overlay_menu(CONTENT_Y_HIGH - OVERLAY_MENU_ICON_SIZE - main_tab_view->tab_selector_height);
		
		if(Util_expl_query_show_flag())
			Util_expl_draw();

		if(Util_err_query_error_show_flag())
			Util_err_draw();

		Draw_touch_pos();

		Draw_apply_draw();
	}
	else
		gspWaitForVBlank();
	

	resource_lock.lock();

	if (Util_err_query_error_show_flag()) {
		Util_err_main(key);
	} else if(Util_expl_query_show_flag()) {
		Util_expl_main(key);
	} else {
		update_overlay_menu(&key, &intent, SceneType::SUBSCRIPTION);
		
		channels_tab_view->update_y_range(0, CONTENT_Y_HIGH - TOP_HEIGHT - main_tab_view->tab_selector_height);
		feed_videos_view->update_y_range(0, CONTENT_Y_HIGH - TOP_HEIGHT - main_tab_view->tab_selector_height - FEED_RELOAD_BUTTON_HEIGHT - SMALL_MARGIN);
		main_view->update(key);
		if (clicked_url != "") {
			intent.next_scene = clicked_is_video ? SceneType::VIDEO_PLAYER : SceneType::CHANNEL;
			intent.arg = clicked_url;
			clicked_url = "";
		}
		if (video_playing_bar_show) video_update_playing_bar(key, &intent);
		
		static int consecutive_scroll = 0;
		if (key.h_c_up || key.h_c_down) {
			if (key.h_c_up) consecutive_scroll = std::max(0, consecutive_scroll) + 1;
			else consecutive_scroll = std::min(0, consecutive_scroll) - 1;
			
			float scroll_amount = DPAD_SCROLL_SPEED0;
			if (std::abs(consecutive_scroll) > DPAD_SCROLL_SPEED1_THRESHOLD) scroll_amount = DPAD_SCROLL_SPEED1;
			if (key.h_c_up) scroll_amount *= -1;
			
			(main_tab_view->selected_tab == 0 ? channels_tab_view : feed_videos_view)->scroll(scroll_amount);
			var_need_reflesh = true;
		} else consecutive_scroll = 0;
		
		if(key.h_touch || key.p_touch)
			var_need_reflesh = true;
		
		if (key.p_b) intent.next_scene = SceneType::BACK;
	}
	resource_lock.unlock();

	if(Util_log_query_log_show_flag())
		Util_log_main(key);
	
	if (key.p_select) Util_log_set_log_show_flag(!Util_log_query_log_show_flag());
	
	return intent;
}
