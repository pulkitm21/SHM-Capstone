import "./App.css";
import Layout from "./Layout/Layout";
import { BrowserRouter, Routes, Route, Navigate } from "react-router-dom";
import Home from "./Pages/Home/Home";
import Users from "./Pages/Users/Users";
import SensorControl from "./Pages/SensorControl/SensorControl";
import Export from "./Pages/Export/Export";

function App() {
  return (
   <BrowserRouter>
      <Routes>
        <Route path="/" element={<Layout />}>
          <Route index element={<Home />} />
          <Route path="/sensorcontrol" element={<SensorControl />} />
          <Route path="/export" element={<Export />} />
          <Route path="/users" element={<Users />} />
          <Route path="*" element={<Navigate to="/" replace />} />
        </Route>
      </Routes>
    </BrowserRouter>
  );
}
export default App;
