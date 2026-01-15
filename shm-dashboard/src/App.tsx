import './App.css';
import Navbar from './components/Navbar/Navbar';
import Topbar from './components/TopBar/TopBar';

function App() {
  return (
    <div>
      {
        <Navbar />
      }
      {
        <Topbar />
      }
    </div>
  );
}

export default App;