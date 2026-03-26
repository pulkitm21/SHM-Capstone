import "./App.css";
import Layout from "./Layout/Layout";
import { BrowserRouter, Routes, Route, Navigate } from "react-router-dom"; // Used for routing between pages

import Home from "./Pages/Home/Home";
import SensorManagement from "./Pages/SensorManagement/SensorManagement";
import ServerManagement from "./Pages/ServerManagement/ServerManagement";
import Export from "./Pages/Export/Export";
import FaultLog from "./Pages/FaultLog/FaultLog";
import Decoder from "./Pages/Decoder/Decoder";

import Login from "./Pages/Login/Login";
import ProtectedRoute from "./Auth/Route";

function App() {
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/login" element={<Login />} />

        <Route element={<ProtectedRoute />}>
          {/* ProtectedRoute will check if the user is authenticated before rendering its child routes */}
          <Route path="/" element={<Layout />}>
            <Route index element={<Home />} />
            <Route path="sensor-management" element={<SensorManagement />} />
            <Route path="server-management" element={<ServerManagement />} />
            <Route path="fault-log" element={<FaultLog />} />
            <Route path="export" element={<Export />} />
            <Route path="decoder" element={<Decoder />} />
            <Route path="*" element={<Navigate to="/" replace />} />
          </Route>
        </Route>
      </Routes>
    </BrowserRouter>
  );
}

export default App;