#!/usr/bin/env python3
"""
Real Browser Verification for Distributed LSM-Tree Web GUI Dashboard.
Controls real Chromium/Edge browser through WebDriver.
Executes all 6 user verification steps, validates visible DOM, and saves screenshots.
"""

import json
import os
import re
import sys
import time

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8')

from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.support.ui import WebDriverWait, Select
from selenium.webdriver.support import expected_conditions as EC
from selenium.webdriver.chrome.options import Options as ChromeOptions
from selenium.webdriver.edge.options import Options as EdgeOptions

SCREENSHOT_DIR = os.path.join(os.path.dirname(__file__), "screenshots")
os.makedirs(SCREENSHOT_DIR, exist_ok=True)


def get_driver():
    # Try Edge first, then Chrome
    edge_opts = EdgeOptions()
    edge_opts.add_argument("--headless=new")
    edge_opts.add_argument("--window-size=1366,768")
    edge_opts.add_argument("--disable-gpu")
    edge_opts.add_argument("--no-sandbox")

    try:
        driver = webdriver.Edge(options=edge_opts)
        driver.set_window_size(1366, 768)
        print(" [OK] Initialized Microsoft Edge WebDriver (headless, 1366x768)")
        return driver
    except Exception as e_edge:
        print(f" Edge failed ({e_edge}), falling back to Chrome...")
        chrome_opts = ChromeOptions()
        chrome_opts.add_argument("--headless=new")
        chrome_opts.add_argument("--window-size=1366,768")
        chrome_opts.add_argument("--disable-gpu")
        chrome_opts.add_argument("--no-sandbox")
        driver = webdriver.Chrome(options=chrome_opts)
        driver.set_window_size(1366, 768)
        print(" [OK] Initialized Chrome WebDriver (headless, 1366x768)")
        return driver


def verify_browser():
    driver = get_driver()
    wait = WebDriverWait(driver, 10)

    try:
        print("\n==================================================================")
        print(" Step 1: Open http://localhost:8080 & Confirm 3 Node Cards + Leader")
        print("==================================================================")
        driver.get("http://localhost:8080")
        time.sleep(1.5)

        # Bootstrap cluster to ensure clean initial state
        driver.find_element(By.ID, "btnStartCluster").click()
        time.sleep(3.0)
        driver.find_element(By.ID, "btnRefresh").click()
        time.sleep(1.5)

        # Confirm title and header
        title = driver.title
        print(f" Page Title: '{title}'")
        assert "Distributed LSM-Tree" in title

        # Check Connection Indicator
        conn_label = driver.find_element(By.ID, "connLabel").text
        print(f" Connection Label: '{conn_label}'")
        assert "Online" in conn_label or "Nodes" in conn_label

        # Check Leader summary
        leader_text = driver.find_element(By.ID, "valLeader").text
        term_text = driver.find_element(By.ID, "valTerm").text
        print(f" Cluster Leader: '{leader_text}', Raft Term: '{term_text}'")
        assert "Node" in leader_text

        # Confirm 3 Node Cards
        node_cards = driver.find_elements(By.CSS_SELECTOR, ".node-card")
        print(f" Rendered Node Cards Count: {len(node_cards)}")
        assert len(node_cards) == 3

        leader_found = False
        followers_found = 0
        for i, card in enumerate(node_cards):
            card_text = card.text
            is_leader = "LEADER" in card_text.upper() or "Leader" in card_text
            is_follower = "FOLLOWER" in card_text.upper() or "Follower" in card_text
            if is_leader:
                leader_found = True
            if is_follower:
                followers_found += 1
            print(f"  -> Card {i+1}: {card.find_element(By.CSS_SELECTOR, '.node-name').text} | Badge: {card.find_element(By.CSS_SELECTOR, '.badge').text}")

        assert leader_found, "Leader badge must appear on active leader card"
        assert followers_found >= 1, "Follower badges must appear on follower cards"
        print(" [PASS] Step 1: All 3 node cards and leader status confirmed in browser.")

        driver.save_screenshot(os.path.join(SCREENSHOT_DIR, "step1_cluster_overview.png"))

        print("\n==================================================================")
        print(" Step 2: PUT a value through the form, then GET it through form")
        print("==================================================================")
        # Ensure PUT tab active
        put_tab = driver.find_element(By.CSS_SELECTOR, "button[data-op='put']")
        put_tab.click()

        key_input = driver.find_element(By.ID, "kvKey")
        val_input = driver.find_element(By.ID, "kvValue")
        submit_btn = driver.find_element(By.ID, "btnSubmitKv")

        # 2a: PUT sensor:turbofan:temperature = 1420.5
        key_input.clear()
        key_input.send_keys("sensor:turbofan:temperature")
        val_input.clear()
        val_input.send_keys("1420.5")
        submit_btn.click()

        # Wait for result box
        time.sleep(1.5)
        res_badge = driver.find_element(By.ID, "resStatusBadge").text
        res_content = driver.find_element(By.ID, "resContent").text
        res_latency = driver.find_element(By.ID, "resLatency").text
        print(f" PUT Response Badge: '{res_badge}' | Content: '{res_content}' | Latency: '{res_latency}'")
        assert res_badge == "OK"

        driver.save_screenshot(os.path.join(SCREENSHOT_DIR, "step2a_put_response.png"))

        # 2b: GET sensor:turbofan:temperature
        get_tab = driver.find_element(By.CSS_SELECTOR, "button[data-op='get']")
        get_tab.click()
        time.sleep(0.3)

        # Notice groupValue should be hidden for GET
        group_val = driver.find_element(By.ID, "groupValue")
        assert "hidden" in group_val.get_attribute("class")

        key_input = driver.find_element(By.ID, "kvKey")
        key_input.clear()
        key_input.send_keys("sensor:turbofan:temperature")
        submit_btn.click()

        time.sleep(1.5)
        res_badge = driver.find_element(By.ID, "resStatusBadge").text
        res_content = driver.find_element(By.ID, "resContent").text
        print(f" GET Response Badge: '{res_badge}' | Content: '{res_content}'")
        assert res_badge == "FOUND"
        assert "1420.5" in res_content
        print(" [PASS] Step 2: PUT and GET completed successfully through visible form.")

        driver.save_screenshot(os.path.join(SCREENSHOT_DIR, "step2b_get_response.png"))

        print("\n==================================================================")
        print(" Step 3: Send request to a FOLLOWER and confirm LEADER REDIRECT")
        print("==================================================================")
        # Identify leader node ID
        leader_num = leader_text.replace("Node", "").strip()
        follower_port = "50052" if leader_num != "2" else "50051"
        follower_endpoint = f"127.0.0.1:{follower_port}"
        print(f" Current Leader is Node {leader_num}. Targeting FOLLOWER at {follower_endpoint}...")

        # Switch to PUT tab
        put_tab.click()
        select_target = Select(driver.find_element(By.ID, "targetEndpoint"))
        select_target.select_by_value(follower_endpoint)

        key_input.clear()
        key_input.send_keys("telemetry:vibration:p99")
        val_input.clear()
        val_input.send_keys("0.024mm")
        submit_btn.click()

        time.sleep(1.5)
        res_badge = driver.find_element(By.ID, "resStatusBadge").text
        res_handling = driver.find_element(By.ID, "resHandling").text
        print(f" Follower PUT Badge: '{res_badge}' | Handling Node: '{res_handling}'")
        assert res_badge == "OK"
        assert "Redirected" in res_handling or f"Leader {leader_num}" in res_handling
        print(f" [PASS] Step 3: Verified explicit leader redirection in UI: '{res_handling}'")

        driver.save_screenshot(os.path.join(SCREENSHOT_DIR, "step3_follower_redirect.png"))

        print("\n==================================================================")
        print(" Step 4: DELETE the value and confirm subsequent GET reports NOT_FOUND")
        print("==================================================================")
        del_tab = driver.find_element(By.CSS_SELECTOR, "button[data-op='delete']")
        del_tab.click()
        time.sleep(0.3)

        key_input = driver.find_element(By.ID, "kvKey")
        key_input.clear()
        key_input.send_keys("telemetry:vibration:p99")
        submit_btn.click()

        time.sleep(1.5)
        res_badge = driver.find_element(By.ID, "resStatusBadge").text
        print(f" DELETE Response Badge: '{res_badge}'")
        assert res_badge == "OK"

        # Subsequent GET for deleted key
        get_tab.click()
        time.sleep(0.3)
        key_input.clear()
        key_input.send_keys("telemetry:vibration:p99")
        submit_btn.click()

        time.sleep(1.5)
        res_badge = driver.find_element(By.ID, "resStatusBadge").text
        res_content = driver.find_element(By.ID, "resContent").text
        print(f" Post-Delete GET Badge: '{res_badge}' | Content: '{res_content}'")
        assert res_badge == "NOT_FOUND"
        assert "not found" in res_content.lower()
        print(" [PASS] Step 4: DELETE succeeded and subsequent GET verified NOT_FOUND in UI.")

        driver.save_screenshot(os.path.join(SCREENSHOT_DIR, "step4_delete_and_not_found.png"))

        print("\n==================================================================")
        print(" Step 5: Stop a node / cluster -> Verify Unhealthy state -> Restart")
        print("==================================================================")
        # 5a: Click visible 'Stop Cluster' header button to simulate cluster outage
        print(" Clicking visible 'Stop Cluster' header button...")
        stop_cluster_btn = driver.find_element(By.ID, "btnStopCluster")
        stop_cluster_btn.click()

        # Wait for cluster to reflect offline in visible UI
        time.sleep(3.0)
        driver.find_element(By.ID, "btnRefresh").click()
        time.sleep(1.5)

        conn_label_stopped = driver.find_element(By.ID, "connLabel").text
        leader_stopped = driver.find_element(By.ID, "valLeader").text
        print(f" Connection Status after Stop: '{conn_label_stopped}', Leader: '{leader_stopped}'")
        assert "Offline" in conn_label_stopped or "0" in conn_label_stopped

        # Verify visible node cards show offline/unreachable
        offline_cards = 0
        for card in driver.find_elements(By.CSS_SELECTOR, ".node-card"):
            txt = card.text.upper()
            badge = card.find_element(By.CSS_SELECTOR, ".badge").text.upper()
            if "OFFLINE" in badge or "UNREACHABLE" in txt or "offline" in card.get_attribute("class"):
                offline_cards += 1
        print(f" Offline/Unreachable Node Cards Count: {offline_cards}/3")
        assert offline_cards >= 1

        driver.save_screenshot(os.path.join(SCREENSHOT_DIR, "step5a_cluster_stopped.png"))

        # 5b: Click visible 'Start Cluster' header button to recover
        print(" Clicking visible 'Start Cluster' header button...")
        start_cluster_btn = driver.find_element(By.ID, "btnStartCluster")
        start_cluster_btn.click()

        # Wait for cluster to bootstrap and recover
        time.sleep(4.0)
        driver.find_element(By.ID, "btnRefresh").click()
        time.sleep(2.0)

        conn_label_recovered = driver.find_element(By.ID, "connLabel").text
        leader_recovered = driver.find_element(By.ID, "valLeader").text
        print(f" Connection Status after Restart: '{conn_label_recovered}', Leader: '{leader_recovered}'")
        assert "Online" in conn_label_recovered

        # Confirm all 3 cards recovered
        online_cards = 0
        for card in driver.find_elements(By.CSS_SELECTOR, ".node-card"):
            badge = card.find_element(By.CSS_SELECTOR, ".badge").text.upper()
            if badge in ["LEADER", "FOLLOWER"]:
                online_cards += 1
        print(f" Online Node Cards Count after recovery: {online_cards}/3")
        assert online_cards == 3
        print(" [PASS] Step 5: Stop cluster and restart recovery confirmed live in visible browser UI.")

        driver.save_screenshot(os.path.join(SCREENSHOT_DIR, "step5b_cluster_recovered.png"))

        print("\n==================================================================")
        print(" Step 6: Layout Viewport & Strict Color Palette Audit")
        print("==================================================================")
        # Check viewport
        window_size = driver.get_window_size()
        print(f" Browser Window Size: {window_size['width']}x{window_size['height']}")
        assert window_size['width'] >= 1280

        # Audit computed styles across elements
        body = driver.find_element(By.TAG_NAME, "body")
        bg_color = body.value_of_css_property("background-color")
        color = body.value_of_css_property("color")
        print(f" Body Background: '{bg_color}', Text Color: '{color}'")

        # Scan all elements on page to verify NO purple, blue, indigo, pink, or gradient
        elements = driver.find_elements(By.CSS_SELECTOR, "*")
        print(f" Inspecting {len(elements)} DOM elements for color/gradient violations...")

        forbidden_colors_found = []
        for el in elements:
            try:
                bg = el.value_of_css_property("background-color")
                bg_image = el.value_of_css_property("background-image")
                txt_color = el.value_of_css_property("color")

                if "gradient" in bg_image.lower():
                    forbidden_colors_found.append(f"Gradient in {el.tag_name}: {bg_image}")

                # Check for blue/purple in RGB
                # In rgb(r, g, b), high blue (>170) with low red and green indicates blue/purple
                for c_str in [bg, txt_color]:
                    if c_str.startswith("rgb"):
                        nums = [int(n.strip()) for n in re.findall(r"\d+", c_str)[:3]]
                        if len(nums) == 3:
                            r, g, b = nums
                            # Pure/strong blue check: b > 160 and b > r + 40 and b > g + 40
                            if b > 160 and b > r + 40 and b > g + 30:
                                forbidden_colors_found.append(f"Blue color found in {el.tag_name}: {c_str}")
                            # Purple/indigo check: r > 100 and b > 140 and g < 100
                            if r > 100 and b > 140 and g < 90:
                                forbidden_colors_found.append(f"Purple/indigo color found in {el.tag_name}: {c_str}")
                            # Pink check: r > 200 and b > 150 and g < 150
                            if r > 200 and b > 150 and g < 140:
                                forbidden_colors_found.append(f"Pink color found in {el.tag_name}: {c_str}")
            except Exception:
                pass

        if forbidden_colors_found:
            print(" Violations detected:", forbidden_colors_found[:5])
        assert len(forbidden_colors_found) == 0, f"Found color violations: {forbidden_colors_found[:5]}"
        print(" [PASS] Step 6: Layout is verified at laptop viewport (1366x768). Zero purple, blue, indigo, pink, or gradients detected.")

        driver.save_screenshot(os.path.join(SCREENSHOT_DIR, "step6_final_layout_verified.png"))

        print("\n==================================================================")
        print(" ALL 6 VISIBLE BROWSER VERIFICATION STEPS PASSED SUCCESSFULLY!")
        print("==================================================================")

    finally:
        driver.quit()


if __name__ == "__main__":
    verify_browser()
