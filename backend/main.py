from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
import csv
from pathlib import Path
 
app = FastAPI()
 
# CORS (already discussed)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:5173"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)
 
DATA_PATH = Path("/home/pi/Data/accel_data.csv")
 
 
@app.get("/api/accel")
def get_accel_data():
    if not DATA_PATH.exists():
        return {
            "sensor": "accelerometer",
            "unit": "g",
            "points": []
        }
 
    points = []
 
    with open(DATA_PATH, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                points.append({
                    "ts": row["timestamp"],
                    "value": float(row["ax"])  # using X-axis
                })
            except Exception:
                continue
 
    # optional: only last 500 points
    points = points[-500:]
 
    return {
        "sensor": "accelerometer",
        "unit": "g",
        "points": points
    }

@app.get("/")
def root():
	return {"message": "backend working"}

@app.get("/health")
def health():
	return {"status": "OK"}

