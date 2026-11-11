import streamlit as st
import streamlit.components.v1 as components
import requests
import time
from dotenv import load_dotenv
import os
import pandas as pd

# ---------------------------------------------------------------------------
# Config / backend URLs  (same pattern as app.py)
# ---------------------------------------------------------------------------
load_dotenv(override=True)
base_url = str(os.getenv("BASE_URL"))
predict_url = base_url + "predict"
upload_url = base_url + "upload"
get_latest_data_url = base_url + "get_latest_data"
get_all_data_url = base_url + "get_all_data"
set_anchor_location_url = base_url + "set_anchor_location"

st.set_page_config(page_title="ForgetMeNot", layout="wide")

st.markdown(
    """
    <style>
    /* Persistent title, top-left */
    .fmn-title {
        position: fixed;
        top: 0.5rem;          /* sit within the header band, top-left */
        left: 1rem;
        z-index: 1000000;     /* render on top of Streamlit's header */
        font-size: 28px;
        font-weight: 800;
        color: #4a6cf7;
        background: rgba(255, 255, 255, 0.9);
        padding: 4px 10px;
        border-radius: 8px;
    }
    /* Push page content down so it isn't hidden behind the fixed title */
    .block-container {
        padding-top: 90px;
    }

    [data-testid="stMetricValue-small"] { font-size: 20px; }
    .metric-label { font-size: 12px; color: gray; }
    .metric-value { font-size: 20px; }
    .icon-container {
        display: flex;
        justify-content: center;
        align-items: center;
        margin-top: 15px;
        height: 100%;
    }
    .icon { font-size: 50px; }
    .metric-container {
        display: flex;
        justify-content: center;
        align-items: center;
        width: 100%;
    }
    </style>
    """,
    unsafe_allow_html=True,
)

st.markdown(
    '<div class="fmn-title">ForgetMeNot</div>',
    unsafe_allow_html=True,
)

def fetch_data():
    """Fetch all historical rows from the database."""
    response = requests.get(get_all_data_url)
    if response.status_code == 200:
        return response.json()
    st.error(f"Error: Unable to fetch data {response}")
    return None

# space between elements
def spacer(n):
    for _ in range(n):
        st.write("")


# set anchor location for distance calculation
def anchor_location_section():
    st.subheader("🏠 Anchor Location")

    options = {
        "Location 1 (Home)": 1,
        "Location 2 (Secondary)": 2,
    }

    choice = st.selectbox("Choose anchor location", list(options.keys()))
    loc_key = options[choice]

    if st.button("Set anchor"):
        try:
            resp = requests.post(set_anchor_location_url, json={"loc_key": loc_key})
            if resp.status_code == 200:
                anchor = resp.json()
                st.session_state["anchor"] = anchor
                st.success(
                    f"Anchor set to {anchor['anchor_lat']}, {anchor['anchor_lon']}"
                )
            else:
                st.error(f"Failed to set anchor ({resp.status_code}): {resp.text}")
        except requests.RequestException as e:
            st.error(f"Request error: {e}")

    if "anchor" in st.session_state:
        a = st.session_state["anchor"]
        st.caption(f"Current anchor: {a['anchor_lat']}, {a['anchor_lon']}")


# display current location and vital signs
def display_data(value, label, metric):
    """Render a single metric with an icon (carried over from app.py)."""
    pad, icon, met = st.columns([0.1, 0.2, 0.7])
    with pad:
        st.write(" ")
    with icon:
        icons = {
            "Body Temperature": "🌡️",
            "SPO2": "🩸",
            "Heart Rate": "💓",
            "Speed": "🏃🏼‍♂️",
            "Latitude": "🧭",
            "Longitude": "🧭",
        }
        if label in icons:
            st.markdown(
                f"<div class='icon-container'><div class='icon'>{icons[label]}</div></div>",
                unsafe_allow_html=True,
            )
    with met:
        st.metric(label, value=f"{value:.2f} {metric}", delta_color="normal")


def map_and_latest_section(latest_data):
    map_col, data_col = st.columns([0.6, 0.4])

    lat = list(latest_data["latitude"])[-1]
    lon = list(latest_data["longitude"])[-1]

    with map_col:
        st.subheader("📍 Live Location")
        gmap_url = f"https://maps.google.com/maps?q={lat},{lon}&z=16&output=embed"
        components.iframe(gmap_url, height=450)

    with data_col:
        st.subheader("📊 Latest Data")
        row = latest_data.iloc[-1]
        display_data(float(row["body_temp"]), "Body Temperature", "°C")
        display_data(float(row["heart_rate"]), "Heart Rate", "bpm")
        display_data(float(row["spo2"]), "SPO2", "%")
        display_data(float(row["speed"]), "Speed", "km/h")
        display_data(float(row["latitude"]), "Latitude", "")
        display_data(float(row["longitude"]), "Longitude", "")


# prediction
def display_risk():
    st.subheader("🧠 Predicted Condition")
    resp = requests.get(predict_url)
    if resp.status_code == 404:
        st.error("Error: No data available. Please check the sensor connection.")
        return

    risk_level = resp.json()["risk_level"]

    color_map = {
        "Low Risk": "#00cc44", "Normal": "#00cc44",
        "Medium Risk": "#ffcc00", "Warning": "#ffcc00",
        "High Risk": "#ff3333", "Danger": "#ff3333",
    }
    icon_map = {
        "Low Risk": "🟢", "Normal": "🟢",
        "Medium Risk": "🟡", "Warning": "🟡",
        "High Risk": "🔴", "Danger": "🔴",
    }
    color = color_map.get(risk_level, "#888888")
    icon = icon_map.get(risk_level, "⚪")

    st.markdown(
        f"""
        <style>
        .risk-container {{
            display: flex; flex-direction: column; align-items: center;
            border: 2px solid {color}; border-radius: 10px;
            padding: 20px; margin: 10px;
        }}
        .risk-category {{ font-size: 30px; font-weight: bold; color: {color}; }}
        .risk-icon {{ font-size: 60px; }}
        </style>
        <div class="risk-container">
            <div class="risk-icon">{icon}</div>
            <div class="risk-category">{risk_level}</div>
        </div>
        """,
        unsafe_allow_html=True,
    )


# past data visualization based on time frame
def graph_menu(data):
    st.subheader("📈 History")
    graph_col1, graph_col2 = st.columns([0.2, 0.8])

    with graph_col1:
        spo2_check = st.checkbox("SPO2", value=True, key="spo2")
        hr_check = st.checkbox("Heart Rate", value=True, key="heart_rate")
        body_temp_check = st.checkbox("Body Temperature", value=True, key="body_temp")
        speed_check = st.checkbox("Speed", value=True, key="speed")

        selected_columns = []
        if spo2_check:
            selected_columns.append("spo2")
        if hr_check:
            selected_columns.append("heart_rate")
        if body_temp_check:
            selected_columns.append("body_temp")
        if speed_check:
            selected_columns.append("speed")
        selected_columns.append("timestamp")

        selected_data = data.loc[:, selected_columns].copy()
        ts = pd.to_datetime(selected_data["timestamp"])
        if ts.dt.tz is not None:
            ts = ts.dt.tz_localize(None)
        selected_data["timestamp"] = ts
        selected_data.set_index("timestamp", inplace=True)

        timeframes = ["30 minutes","1 Hour", "1 Day", "1 Month"]
        timeframe = st.selectbox("Select Timeframe", timeframes)

        now = pd.Timestamp.now()
        if timeframe == "30 minutes":
            selected_data = selected_data[selected_data.index >= now - pd.DateOffset(minutes=30)]
        elif timeframe == "1 Hour":
            selected_data = selected_data[selected_data.index >= now - pd.DateOffset(hours=1)]
        elif timeframe == "1 Day":
            selected_data = selected_data[selected_data.index >= now - pd.DateOffset(days=1)]
        elif timeframe == "1 Month":
            selected_data = selected_data[selected_data.index >= now - pd.DateOffset(months=1)]

    with graph_col2:
        st.line_chart(data=selected_data)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    # 1. Anchor location dropdown (top component)
    anchor_location_section()
    st.divider()

    while True:
        data = fetch_data()
        latest_data = requests.get(get_latest_data_url).json()
        data = pd.DataFrame(data)
        latest_data = pd.DataFrame([latest_data])

        # 2. Map + latest data
        map_and_latest_section(latest_data)
        spacer(2)
        st.divider()

        if len(data) == 0:
            st.write("No data available. Please check the sensor connection.")
        else:
            # 3. Prediction
            display_risk()
            spacer(2)
            st.divider()

            # 4. History visualization
            graph_menu(data)

        time.sleep(10)
        st.rerun()


if __name__ == "__main__":
    main()
