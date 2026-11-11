from flask import Flask, request, jsonify
import numpy as np
import pickle
from pymongo.mongo_client import MongoClient
from pymongo.server_api import ServerApi
import datetime
from dotenv import load_dotenv
import os
import pymongo
from geopy.distance import geodesic

anchor_dict = {
    "home":{            
        'anchor_lat': -6.191389, 
        'anchor_lon': 106.781389
    },
    "work":{
        'anchor_lat': -6.20201, 
        'anchor_lon': 106.78113
    }
}

anchor_flag = 1

load_dotenv(override=True)
uri = os.getenv('MONGO_URI')
client = MongoClient(uri)
db = client['ForgetMeNot_db']
collection = db['sensor_data']

model = pickle.load(open('sensor_model.pkl', 'rb'))
app = Flask(__name__)

def get_data(pipeline: list[dict]) -> list[dict]:
    result = list(collection.aggregate(pipeline))
    return result

# get last 5 data
def get_avg_data()->dict:
    pipeline = [
        {"$sort": {"timestamp": -1}},
        {"$limit": 5},
        {
            "$group": {
                "_id": None,
                "average_body_temp": {"$avg": "$body_temp"},
                "average_heart_rate": {"$avg": "$heart_rate"},
                "average_spo2": {"$avg": "$spo2"}
            }
        }
    ]
    result = get_data(pipeline)[0]
    return result

def check_distance(latitude, longitude):
    if anchor_flag == 1:
        anchor_coords = (anchor_dict['home']['anchor_lat'], anchor_dict['home']['anchor_lon'])
        distance = geodesic(anchor_coords, (latitude, longitude)).meters
    else:
        anchor_coords = (anchor_dict['work']['anchor_lat'], anchor_dict['work']['anchor_lon'])
        distance = geodesic(anchor_coords, (latitude, longitude)).meters
    return distance


def make_prediction(features, latitude, longitude):
    distance_from_anchor = check_distance(latitude, longitude)

    if (distance_from_anchor > 500):
        return 2
    elif (distance_from_anchor > 100 and distance_from_anchor <= 500):
        return 1
    else:
        prediction = model.predict(features)
        return prediction

# flask routing
@app.route('/')
def backend_check():
    return jsonify({"status": "ok", "service": "mainfeat"}), 200


@app.route('/upload', methods=['POST'])
def upload():
    latest_data = request.get_json(force=True)
    data_to_send = {
        "timestamp": datetime.datetime.now(),
        "body_temp": latest_data['body_temp'],
        "heart_rate": latest_data['heart_rate'],
        "spo2": latest_data['spo2'],
        "latitude": latest_data['latitude'],
        "longitude": latest_data['longitude'],
        "speed": latest_data['speed']
    }
    collection.insert_one(data_to_send)
    return jsonify({"status": "Data received"}), 200

# for dashboard
@app.route('/get_latest_data', methods=["GET"])
def get_latest_data():
    pipeline = [
        {"$sort": {"timestamp": -1}},
        {"$limit": 1}
    ]
    latest_data = get_data(pipeline)[0]
    latest_data.pop("_id")
    return jsonify(latest_data)

@app.route('/get_all_data', methods=['GET'])
def get_all_data():
    data = list(collection.find())
    for d in data:
        d.pop("_id")
    return jsonify(data)

@app.route('/set_anchor_location', methods=['POST'])
def set_anchor_location():
    global anchor_flag
    req = request.get_json(force=True)
    if int(req['loc_key']) == 1:
        anchor_flag = 1
        return jsonify(anchor_dict['home'])
    else:
        anchor_flag = 2
        return jsonify(anchor_dict['work'])

# automation
@app.route('/predict', methods=['GET'])
def predict_risk_level():
    features = get_avg_data()
    latest_data = get_data([{"$sort": {"timestamp": -1}},{"$limit": 1}])[0]
    latitude, longitude = latest_data['latitude'], latest_data['longitude']
    if features is None:
        return jsonify({"error": "No data in database"}), 404
    np_features = np.array([features['average_spo2'], features['average_heart_rate'], features['average_body_temp']]).reshape(1, -1)
    prediction = make_prediction(np_features, latitude, longitude)
    output = int(prediction)

    risk_levels = {0: 'Normal', 1: 'Warning', 2: 'Danger'}
    risk_output = risk_levels.get(output, "Unknown Risk Level")

    return jsonify({"risk_level": risk_output}), 200



if __name__ == "__main__":
    app.run(host='0.0.0.0', debug=True)
