import "./App.css";
import Layout from "./Layout/Layout";
import { BrowserRouter, Routes, Route, Navigate } from "react-router-dom"; // Used for routing between pages

import Home from "./Pages/Home/Home";
import Export from "./Pages/Export/Export";

import Login from "./Pages/Login/Login";
import ProtectedRoute from "./Auth/Route";


function App() {
  return (
   <BrowserRouter>
      <Routes>

        <Route path="/login" element={<Login />} />

        <Route element={<ProtectedRoute />}> {/* ProtectedRoute will check if the user is authenticated before rendering its child routes */}
        
          <Route path="/" element={<Layout />}>
            <Route index element={<Home />} />
            <Route path="/export" element={<Export />} />
            <Route path="*" element={<Navigate to="/" replace />} />
          </Route>
        </Route>
      </Routes>
    </BrowserRouter>
  );
}
export default App;
