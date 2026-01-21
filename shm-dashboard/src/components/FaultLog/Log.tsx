import './Log.css';

interface LogData {
  id: string;
  title: string;
  level: 'Minor' | 'Intermediate' | 'Major';
  date: string;
}