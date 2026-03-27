import "./App.css";
import Layout from "./Layout/Layout";
import { BrowserRouter, Routes, Route, Navigate } from "react-router-dom";

import { AuthProvider } from "./Auth/AuthContext";
import ProtectedRoute from "./Auth/ProtectedRoute";
import AdminRoute from "./Auth/AdminRoute";

import Home from "./Pages/Home/Home";
import SensorManagement from "./Pages/SensorManagement/SensorManagement";
import ServerManagement from "./Pages/ServerManagement/ServerManagement";
import Export from "./Pages/Export/Export";
import FaultLog from "./Pages/FaultLog/FaultLog";
import Decoder from "./Pages/Decoder/Decoder";
import Login from "./Pages/Login/Login";
import Users from "./Pages/Users/Users";

/*
  The app is wrapped in AuthProvider so authentication state is available
  across the full route tree.
*/
function App() {
  return (
    <AuthProvider>
      <BrowserRouter>
        <Routes>
          {/* Public login route */}
          <Route path="/login" element={<Login />} />

          {/* All authenticated routes are nested under ProtectedRoute */}
          <Route element={<ProtectedRoute />}>
            <Route path="/" element={<Layout />}>
              <Route index element={<Home />} />
              <Route path="sensor-management" element={<SensorManagement />} />
              <Route path="server-management" element={<ServerManagement />} />
              <Route path="fault-log" element={<FaultLog />} />
              <Route path="export" element={<Export />} />
              <Route path="decoder" element={<Decoder />} />

              {/* Admin-only routes */}
              <Route element={<AdminRoute />}>
                <Route path="users" element={<Users />} />
              </Route>

              <Route path="*" element={<Navigate to="/" replace />} />
            </Route>
          </Route>
        </Routes>
      </BrowserRouter>
    </AuthProvider>
  );
}

export default App;